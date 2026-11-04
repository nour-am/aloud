--[[--
Controls for Aloud, the standalone Kindle audiobook player, while it plays in the
background and you read in KOReader.

Each action writes one line ("toggle", "back", "fwd", "prev", "next") to the FIFO Aloud
listens on, /var/tmp/aloud.ctl, then shows what Aloud reports in /var/tmp/aloud.state.
Nothing runs between taps: no timers, no polling, so reading is never slowed down.

Tools menu → Aloud audiobook, or bind the actions to gestures in
Settings → Taps and gestures → Gesture manager (they are listed under "General").

@module koplugin.Aloud
--]]--

local Dispatcher = require("dispatcher")
local InfoMessage = require("ui/widget/infomessage")
local Notification = require("ui/widget/notification")
local UIManager = require("ui/uimanager")
local WidgetContainer = require("ui/widget/container/widgetcontainer")
local ffi = require("ffi")
local bit = require("bit")
local _ = require("gettext")
require("ffi/posix_h")
local C = ffi.C

local CONTROL_FIFO = "/var/tmp/aloud.ctl"
local STATUS_FILE = "/var/tmp/aloud.state"

local ACTIONS = {
    { id = "toggle", event = "AloudToggle", title = _("Aloud: play / pause"), menu = _("Play / pause") },
    { id = "back", event = "AloudBack", title = _("Aloud: back 30 s"), menu = _("Back 30 s") },
    { id = "fwd", event = "AloudForward", title = _("Aloud: forward 30 s"), menu = _("Forward 30 s") },
    { id = "prev", event = "AloudPrevChapter", title = _("Aloud: previous chapter"), menu = _("Previous chapter") },
    { id = "next", event = "AloudNextChapter", title = _("Aloud: next chapter"), menu = _("Next chapter") },
}

local function send(cmd)
    -- Is Aloud listening? A write-only, non-blocking open fails at once (ENXIO) when
    -- nobody has the FIFO open for reading, instead of hanging KOReader.
    local probe = C.open(CONTROL_FIFO, bit.bor(C.O_WRONLY, C.O_NONBLOCK))
    if probe < 0 then return false end
    C.close(probe)
    -- Write through a read-write descriptor: with ourselves as a reader the write can
    -- never raise SIGPIPE (which KOReader doesn't ignore) even if Aloud quits right now.
    local fd = C.open(CONTROL_FIFO, bit.bor(C.O_RDWR, C.O_NONBLOCK))
    if fd < 0 then return false end
    local line = cmd .. "\n"
    local n = C.write(fd, line, #line)
    C.close(fd)
    return n == #line
end

local function read_status()
    local f = io.open(STATUS_FILE, "r")
    if not f then return nil end
    local line = f:read("*l")
    f:close()
    if not line then return nil end
    local state, chapter, book = line:match("^([^\t]*)\t([^\t]*)\t(.*)$")
    return state, chapter, book
end

-- Show "Aloud audiobook" first in the Tools tab (reader and file browser). KOReader
-- places menu items in the order its menu_order tables list them and appends any
-- plugin item it doesn't know at the end; naming ours first in those tables is the
-- supported way to move it. Guarded: if a table is missing, the item just stays last.
local function put_first_in_tools(module_name)
    local ok, order = pcall(require, module_name)
    if not ok or type(order) ~= "table" or type(order.tools) ~= "table" then return end
    for _i, id in ipairs(order.tools) do
        if id == "aloud" then return end
    end
    table.insert(order.tools, 1, "aloud")
end
put_first_in_tools("ui/elements/reader_menu_order")
put_first_in_tools("ui/elements/filemanager_menu_order")

local Aloud = WidgetContainer:extend{
    name = "aloud",
    is_doc_only = false,
}

function Aloud:onDispatcherRegisterActions()
    for _i, a in ipairs(ACTIONS) do
        Dispatcher:registerAction("aloud_" .. a.id, {
            category = "none", event = a.event, title = a.title, general = true,
        })
    end
end

function Aloud:init()
    self:onDispatcherRegisterActions()
    self.ui.menu:registerToMainMenu(self)
    for _i, a in ipairs(ACTIONS) do
        self["on" .. a.event] = function()
            self:run(a.id)
            return true
        end
    end
end

function Aloud:run(cmd)
    if not send(cmd) then
        UIManager:show(InfoMessage:new{
            text = _("Aloud isn't playing in the background.\n\nOpen Aloud from KUAL, start a book and tap “Play in background”."),
            timeout = 4,
        })
        return
    end
    -- Aloud applies the command within a few ms; report what it says it is doing.
    UIManager:scheduleIn(0.4, function()
        local state, chapter = read_status()
        local text
        if state == "playing" then
            text = _("Aloud: playing")
        elseif state == "paused" then
            text = _("Aloud: paused")
        else
            text = _("Aloud")
        end
        if chapter and chapter ~= "" then
            text = text .. " · " .. chapter
        end
        Notification:notify(text, Notification.SOURCE_ALWAYS_SHOW)
    end)
end

function Aloud:addToMainMenu(menu_items)
    local items = {}
    for _i, a in ipairs(ACTIONS) do
        table.insert(items, {
            text = a.menu,
            keep_menu_open = true,
            callback = function() self:run(a.id) end,
        })
    end
    menu_items.aloud = {
        text = _("Aloud audiobook"),
        sorting_hint = "tools",
        sub_item_table = items,
    }
end

return Aloud
