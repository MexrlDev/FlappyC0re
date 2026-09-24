--[[
  flappy.lua -- LuaC0re payload for Flappy Bird PS5
]]

local PC_IP        = "__PC_IP__"
local LOG_PORT     = 9027
local SC_PORT_BASE = 5001
local SC_PORT_MAX  = 5020

-- Enable logging only if PC_IP is a real IPv4 address
local HAVE_LOGS = (PC_IP:match("^%d+%.%d+%.%d+%.%d+$") ~= nil)

init_dlsym()
sceMsgDialogTerminate()

local function htons(p) return ((p << 8) | (p >> 8)) & 0xFFFF end
local function inet_addr(s)
    local a,b,c,d = s:match("(%d+)%.(%d+)%.(%d+)%.(%d+)")
    return (d << 24) | (c << 16) | (b << 8) | a
end
local function make_sockaddr_in(port, ip)
    local sa = malloc(16)
    for i = 0, 15 do write8(sa + i, 0) end
    write8(sa + 0, 16); write8(sa + 1, 2)
    write16(sa + 2, htons(port))
    if ip then write32(sa + 4, inet_addr(ip)) end
    return sa
end

local log_sock = -1
local log_sa   = nil

if HAVE_LOGS then
    log_sock = create_socket(AF_INET, SOCK_DGRAM, 0)
    log_sa   = make_sockaddr_in(LOG_PORT, PC_IP)
end

local function ulog(m)
    if HAVE_LOGS and log_sock >= 0 and log_sa then
        syscall.sendto(log_sock, m .. "\n", #m + 1, 0, log_sa, 16)
    end
end
ulog("flappy: starting")

-- ---------- memory ----------
local SC_TARGET = 0x100000
local rw, rx, SC_SIZE = 0, 0, SC_TARGET

do
    local m = syscall.mmap(0, SC_TARGET, 0x7, 0x1002, -1, 0)
    ulog("mmap 1MB -> 0x" .. string.format("%x", m or 0))
    if m and m > 0x10000 then rw, rx = m, m end
end

if rw == 0 then
    local function jit_alloc(size)
        local bfd, rwfd, rxfd = jit_malloc(8), jit_malloc(8), jit_malloc(8)
        local rwa, rxa, nm = jit_malloc(8), malloc(8), jit_malloc(8)
        if bfd == 0 or rwfd == 0 or rxfd == 0 then return 0, 0 end
        jit_write_buffer(nm, "nv4b")
        jit_sceKernelJitCreateSharedMemory(nm, size, 7, bfd)
        local h = jit_read32(bfd)
        if h == 0 then return 0, 0 end
        jit_sceKernelJitCreateAliasOfSharedMemory(h, PROT_READ|PROT_WRITE, rwfd)
        jit_sceKernelJitCreateAliasOfSharedMemory(h, PROT_READ|PROT_EXECUTE, rxfd)
        jit_sceKernelJitMapSharedMemory(jit_read32(rwfd), PROT_READ|PROT_WRITE, rwa)
        local r = jit_read64(rwa)
        if r == 0 then return 0, 0 end
        local mfd = jit_send_recv_fd(jit_read32(rxfd), NEW_JIT_SOCK, NEW_MAIN_SOCK)
        sceKernelJitMapSharedMemory(mfd, PROT_READ|PROT_EXECUTE, rxa)
        return r, read64(rxa)
    end
    for _, size in ipairs({0x100000, 0xC0000, 0x80000}) do
        local r, x = jit_alloc(size)
        if r ~= 0 then rw, rx, SC_SIZE = r, x, size; break end
    end
end

if rw == 0 then error("no RWX memory for shellcode") end
ulog("shellcode dest rw=0x" .. string.format("%x", rw))

-- ---------- shellcode listener ----------
local srv, sc_port = -1, 0
for p = SC_PORT_BASE, SC_PORT_MAX do
    local s = create_socket(AF_INET, SOCK_STREAM, 0)
    if s >= 0 then
        local en = malloc(4); write32(en, 1)
        syscall.setsockopt(s, 0xFFFF, 0x0004, en, 4)
        local sa = make_sockaddr_in(p)
        if syscall.bind(s, sa, 16) == 0 and syscall.listen(s, 1) == 0 then
            srv, sc_port = s, p
            break
        end
        syscall.close(s)
    end
end
if srv < 0 then error("no free shellcode port") end
ulog("SCPORT " .. tostring(sc_port))

-- ---------- receive shellcode ----------
local sa = make_sockaddr_in(sc_port)
local alen = malloc(8); write32(alen, 16)
ulog("awaiting shellcode")
local cfd = syscall.accept(srv, sa, alen)
if cfd < 0 then error("accept failed") end

local total = 0
while total < SC_SIZE do
    local n = syscall.read(cfd, rw + total, SC_SIZE - total)
    if n == 0 then break end
    if n < 0 then error("read error " .. tostring(n)) end
    total = total + n
end
syscall.close(cfd)
syscall.close(srv)
ulog("shellcode received " .. total .. " / " .. SC_SIZE)
if total < 0x20000 then
    error("short receive: got " .. tostring(total) .. " bytes")
end

-- ---------- ext_args ----------
local ext = malloc(0x80)
memset(ext, 0, 0x80)
write64(ext + 0x00, 0xDEAD)
write32(ext + 0x18, log_sock)
write32(ext + 0x1C, -1)
if log_sa then
    for i = 0, 15 do write8(ext + 0x20 + i, read8(log_sa + i)) end
end

ulog("entering shellcode at 0x" .. string.format("%x", rx))
func_wrap(rx)(EBOOT_BASE, SCE_KERNEL_DLSYM, ext)
