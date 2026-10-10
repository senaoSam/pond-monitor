--- Line protocol on UART1, 115200 8N1, lines end in \n (\r ignored).
--
--   ESP32 -> module                     module -> ESP32
--   (module boots)                      READY <version>
--   PING                                PONG csq=<0-31|99> reg=<0|1> ims=<0|1>
--   LINK                                (nothing: "I got your PONG")
--   DIAL <number> [ring_s] [node] [min] ACK, then DONE <outcome> when it ends
--   HANGUP                              (the DIAL in progress ends with DONE)
--   anything else                       ERR <reason>
--
-- outcome: noanswer | answered | busy | rejected | nocarrier | cancelled | busy-local
-- noanswer is the normal alarm: it rang for ring_s and nobody picked up.
-- If someone does answer, node and minutes are read out by TTS before the
-- call ends, so the board needs no speaker.
module(..., package.seeall)

require "cc"
require "audio"
require "pm"
require "pins"

local UART = 1
local DEFAULT_RING_S = 25
local MAX_RING_S = 60

local function send(s)
    if s:sub(1, 4) ~= "PONG" then log.info("dialer.tx", s) end
    uart.write(UART, s .. "\r\n")
end

-- ---- call state -----------------------------------------------------------
-- cc publishes CALL_CONNECTED / CALL_DISCONNECTED separately; fold them into
-- one topic so a single wait can catch whichever comes first.
sys.subscribe("CALL_CONNECTED", function(num) sys.publish("DIALER_CALL", "connected", num) end)
sys.subscribe("CALL_DISCONNECTED", function(r) sys.publish("DIALER_CALL", "disconnected", r) end)

local function waitCall(ms)
    local got, kind, arg = sys.waitUntil("DIALER_CALL", ms)
    return got and kind or nil, arg
end

local calling = false
local hangupRequested = false

-- Elapsed seconds counted here rather than with os.time(): the RTC is unset at
-- boot and the network sets it later, so os.time() can jump by decades.
local up = 0
sys.timerLoopStart(function() up = up + 1 end, 1000)

local REASONS = { BUSY = "busy", ["NO ANSWER"] = "noanswer", ["NO CARRIER"] = "nocarrier" }

local function ring(number, ringS, speech)
    cc.dial(number)
    local deadline = up + ringS
    while true do
        local left = deadline - up
        if left <= 0 or hangupRequested then
            cc.hangUp(number)
            waitCall(10000)
            return hangupRequested and "cancelled" or "noanswer"
        end
        local kind, arg = waitCall(math.min(left, 2) * 1000)  -- wake to see HANGUP
        if kind == "connected" then
            audio.play(7, "TTS", speech, 7, nil, true, 1500)
            waitCall(15000)
            audio.stop()
            cc.hangUp(number)
            waitCall(10000)
            return "answered"
        elseif kind == "disconnected" then
            -- cc also reports "disconnected" whenever a call-list query comes
            -- back empty, which can happen just after ATD before the call is
            -- listed. Only believe it once no call exists.
            if not cc.anyCallExist() then
                -- Ended before ring_s without being answered: the far end
                -- declined it, was busy, or the network refused it.
                return REASONS[arg] or (up < deadline - ringS + 3 and "nocarrier" or "rejected")
            end
        end
    end
end

-- The TTS engine reads simplified Chinese; traditional characters may come out
-- as silence, so the spoken text is kept in simplified.
local function speechFor(node, minutes)
    if not node then return "鱼塘监控警报" end
    if minutes then
        return string.format("鱼塘监控警报，%s 已经 %s 分钟没有回报", node, minutes)
    end
    return "鱼塘监控警报，" .. node .. " 失联"
end

local function dial(args)
    local number, ringS, node, minutes = args[2], tonumber(args[3]), args[4], args[5]
    if not number or not number:match("^%+?%d+$") or #number < 6 or #number > 15 then
        return send("ERR bad number")
    end
    if calling or cc.anyCallExist() then return send("DONE busy-local") end
    ringS = math.max(5, math.min(ringS or DEFAULT_RING_S, MAX_RING_S))

    calling, hangupRequested = true, false
    send("ACK")
    sys.taskInit(function()
        local outcome = ring(number, ringS, speechFor(node, minutes))
        calling = false
        log.warn("dialer.call", number, outcome)
        send("DONE " .. outcome)
    end)
end

-- ---- status ----------------------------------------------------------------
-- Whether a DIAL can work comes down to two things: is it on the LTE network,
-- and is it registered for VoLTE (IMS). Both are kept current here, for PING
-- and for the LED.
--
-- ril hands the "+CIREG: 0, 1, 5" line to the URC path rather than to the
-- AT+CIREG? response callback (the first version read the callback, always got
-- nothing, and reported ims=0 while the trace showed it registered), so the
-- line is caught as a URC.
local imsReg = 0
ril.regUrc("+CIREG", function(data)
    local v = data:match("%+CIREG:%s*%d+%s*,%s*(%d)")
    if v then imsReg = tonumber(v) end
    sys.publish("DIALER_CIREG")
end)
sys.timerLoopStart(function() ril.request("AT+CIREG?") end, 20000)

local function registered() return net.getState() == "REGISTERED" end
local function canCall() return registered() and imsReg == 1 end

local function ping()
    sys.taskInit(function()
        ril.request("AT+CIREG?")  -- fresh, rather than up to 20s old
        sys.waitUntil("DIALER_CIREG", 3000)
        send(string.format("PONG csq=%s reg=%s ims=%d", tostring(net.getRssi() or 99),
                           registered() and "1" or "0", imsReg))
    end)
end

-- ---- LEDs ------------------------------------------------------------------
-- The board's LEDs hang off the module's GPIO and only light when a script
-- drives them. They answer the one question asked by whoever looks at the
-- board: can it ring a phone right now?
--   NET slow blink (0.2s on / 1.8s off)  ready: LTE and VoLTE registered
--   NET fast blink (0.2s / 0.2s)         not ready: a DIAL would fail
--   NET steady on                        calling
--   DTU steady on                        ESP32 link good both ways
--   DTU fast blink                       its PINGs arrive but our PONGs do not
--                                        reach it: green wire (TXD1 -> GPIO18)
--   DTU off                              nothing from the ESP32 for 90s: yellow
--                                        or grey wire, or the ESP32 is down
-- The ESP32 PINGs every 30s and answers each PONG with LINK, so a board
-- carried to site shows within half a minute whether it is wired right.
--
-- The pins are GPIO1/GPIO4 from Luat's own call demo (it needs the VLCD rail
-- on for that bank); that they are this board's NET and DTU LEDs is an
-- assumption until seen lit. The library default, GPIO64 (pio.P2_0), does not
-- exist on this module: pins.setup threw "invalid PIO operation" at load and
-- took the whole dialer into a reboot loop. So the LEDs are set up under
-- pcall (nothing here yields) and are simply off if it fails -- a wrong LED
-- pin must never cost the ability to ring a phone.
local noop = function() end
local setNet, setDtu = noop, noop
local ledOk, ledErr = pcall(function()
    pmd.ldoset(2, pmd.LDO_VLCD)
    setNet = pins.setup(pio.P0_1, 0)
    setDtu = pins.setup(pio.P0_4, 0)
end)
if not ledOk then
    setNet, setDtu = noop, noop
    log.error("dialer.led", "LEDs disabled:", ledErr)
end

sys.taskInit(function()
    while true do
        if calling then
            setNet(1); sys.wait(500)
        elseif canCall() then
            setNet(1); sys.wait(200); setNet(0); sys.wait(1800)
        else
            setNet(1); sys.wait(200); setNet(0); sys.wait(200)
        end
    end
end)

local LINK_TIMEOUT_S = 90  -- three missed PINGs
local lastRxUp, lastLinkUp  -- uptime of the last command, and of the last LINK

local function fresh(t) return t and up - t <= LINK_TIMEOUT_S end

sys.taskInit(function()
    while true do
        if not fresh(lastRxUp) then
            setDtu(0); sys.wait(500)
        elseif fresh(lastLinkUp) then
            setDtu(1); sys.wait(500)
        else
            setDtu(1); sys.wait(150); setDtu(0); sys.wait(150)
        end
    end
end)

-- ---- input ----------------------------------------------------------------
local function handle(line)
    line = line:gsub("[\r\n]", ""):match("^%s*(.-)%s*$")
    if line == "" then return end
    if line ~= "PING" and line ~= "LINK" then log.info("dialer.rx", line) end
    lastRxUp = up
    local args = {}
    for w in line:gmatch("%S+") do args[#args + 1] = w end
    local cmd = args[1]:upper()
    if cmd == "PING" then ping()
    elseif cmd == "LINK" then lastLinkUp = up
    elseif cmd == "DIAL" then dial(args)
    elseif cmd == "HANGUP" then hangupRequested = true
    else send("ERR unknown command") end
end

-- Bytes are buffered until a newline. A line longer than this is noise on the
-- wire (or the wrong baud rate) and is dropped rather than grown forever.
local MAX_LINE = 200
local buf = ""

local function onReceive()
    while true do
        local s = uart.read(UART, "*l")
        if not s or s == "" then break end
        buf = buf .. s
        while true do
            local nl = buf:find("\n", 1, true)
            if not nl then break end
            handle(buf:sub(1, nl - 1))
            buf = buf:sub(nl + 1)
        end
        if #buf > MAX_LINE then buf = "" end
    end
end

pm.wake("dialer")  -- keep the UART awake: the ESP32 may call at any moment
uart.on(UART, "receive", onReceive)
uart.setup(UART, 115200, 8, uart.PAR_NONE, uart.STOP_1)

sys.taskInit(function()
    sys.waitUntil("CALL_READY", 30000)  -- harmless wait if it already fired
    send("READY " .. VERSION)
end)
