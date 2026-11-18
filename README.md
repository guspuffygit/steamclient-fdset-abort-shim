# steamclient-fdset-abort-shim

An `LD_PRELOAD` shim that stops a Linux dedicated server from dying with this line on stderr.

```
*** bit out of range 0 - FD_SETSIZE on fd_set ***: terminated
```

The abort comes from Valve's `linux64/steamclient.so`. The shim was written for and tested on a Project Zomboid 42.22.2 dedicated server. Other servers that load the same library and use Steam P2P networking can hit the same abort.

## Symptoms

- The server exits with SIGABRT and prints the line above.
- A Java server writes no `hs_err` file, because glibc ends the process itself.
- A backtrace of the core shows `__fdelt_chk` called from `steamclient.so`.
- The crash can happen at any fd count. A busy server with few open files is not safe.

## Install

1. Build the shim. It needs only `gcc`.

   ```
   make fdelt_shim.so
   ```

   Without `make`, this command builds the same file.

   ```
   cc -O2 -U_FORTIFY_SOURCE -fno-stack-protector -shared -fPIC -o fdelt_shim.so fdelt_shim.c
   ```

2. Copy `fdelt_shim.so` into the server directory.

3. Preload it. Project Zomboid's `start-server.sh` appends to an existing `LD_PRELOAD`, so either option works.

   - Export the variable before the script runs. This survives a Steam update that rewrites the script.

     ```
     export LD_PRELOAD=/path/to/server/fdelt_shim.so
     ./start-server.sh
     ```

     For a systemd unit, add `Environment=LD_PRELOAD=/path/to/server/fdelt_shim.so`.

   - Or edit the launch line in `start-server.sh`.

     ```
     LD_PRELOAD="${LD_PRELOAD}:${INSTDIR}/fdelt_shim.so:${JSIG}" ./ProjectZomboid64 "$@"
     ```

4. Restart the server. The shim loads only at process start.

5. Confirm the running process mapped it.

   ```
   grep fdelt_shim /proc/$(pgrep -f '^./ProjectZomboid64')/maps
   ```

## What the shim does

`fdelt_shim.so` replaces glibc's `__fdelt_chk`, the range check behind `FD_SET` and `FD_ISSET` in fortified builds.

- An fd from 0 to 1023 gets the same answer glibc gives.
- An fd outside that range gets word 0 instead of an abort. The caller then tests a bit inside its own `fd_set` and carries on.
- The shim prints a line to stderr on suppressed calls 1, 2, 4, 8 and so on.

  ```
  fdelt-shim: fd -1 is outside the fd_set range, suppressed abort #1
  ```

- The shim imports only `write`, so it loads on any glibc.

## Limits

- The shim removes the check for every library in the process, not only `steamclient.so`.
- A Steam P2P socket numbered 1024 or higher survives but never receives events, because `steamclient.so` leaves it out of the `select()` sets. Keep the server's fd count under 1024 where you can.
- An out-of-range fd can read another socket's ready bit and deliver one spurious event. libjingle handles a spurious read or write on a non-blocking socket as a no-op.

## Cause

`steamclient.so` contains a copy of libjingle. Its `talk_base::PhysicalSocketServer::Wait` runs a `select()` loop over every socket the Steam P2P layer has open.

- Before `select()`, the loop skips sockets with `fd >= 1024`.
- After `select()` returns, a second loop calls `__fdelt_chk(fd)` for every listed socket with no range check.
- `__fdelt_chk` aborts the process when `fd < 0` or `fd >= 1024`.

Two things put a bad fd in that list.

| Trigger | Mechanism |
|---|---|
| fd >= 1024 | The process holds 1024 or more open fds when Steam P2P opens a socket. |
| fd -1 | `PhysicalSocketServer::CreateAsyncSocket` adds the new socket to the list with fd -1, releases the lock, and only then calls `socket()`. If `select()` returns on the `Wait` thread inside that window, the second loop reads fd -1. |

The second trigger is a race on every new P2P socket. It needs no fd pressure. A production server hit it with about 200 open fds.

Offsets in the Project Zomboid 42.22.2 `linux64/steamclient.so` (md5 `86c1277186c24c4e08ef32ec4eee6b42`, image base 0).

| Item | Address |
|---|---|
| `PhysicalSocketServer` constructor | `0x237f900` |
| `PhysicalSocketServer::Wait` | `0x237f1c0` |
| Unchecked `__fdelt_chk` call in `Wait` | `0x237f44e` |
| `PhysicalSocketServer::CreateAsyncSocket` | `0x23806d0` |
| `socket()` call in `CreateAsyncSocket` | `0x2380819` |

A core from this build shows the aborting thread at `steamclient.so+0x237f453`.

## Reproduce and verify

`repro.c` loads the real `steamclient.so`, builds the socket server object, and forces each trigger.

| Mode | Method |
|---|---|
| `high-fd` | Fills every fd below 1024, then creates a socket and runs `Wait`. |
| `race` | Interposes `socket()` to park `CreateAsyncSocket`, then runs `Wait` on a second thread. |

```
make
./verify.sh /path/to/server/linux64/steamclient.so
```

Each mode must exit 134 without the shim and 0 with it.

```
PASS  high-fd  shim=no  exit=134
      *** bit out of range 0 - FD_SETSIZE on fd_set ***: terminated
PASS  high-fd  shim=yes  exit=0
      fdelt-shim: fd 1024 is outside the fd_set range, suppressed abort #1
      fdelt-shim: fd 1024 is outside the fd_set range, suppressed abort #2
      fdelt-shim: fd 1024 is outside the fd_set range, suppressed abort #4
      survived
PASS  race  shim=no  exit=134
      *** bit out of range 0 - FD_SETSIZE on fd_set ***: terminated
PASS  race  shim=yes  exit=0
      fdelt-shim: fd -1 is outside the fd_set range, suppressed abort #1
      fdelt-shim: fd -1 is outside the fd_set range, suppressed abort #2
      survived
```

`repro.c` hard-codes the offsets above. It exits 2 on any other `steamclient.so` build. The shim itself has no build-specific offsets.

Tested on glibc 2.39 (Ubuntu 24.04) and glibc 2.43.

## Valve status

Valve tracks the fd >= 1024 trigger in [ValveSoftware/steam-for-linux#13843](https://github.com/ValveSoftware/steam-for-linux/issues/13843).

| Binary | Build date | State |
|---|---|---|
| `steamrt64/steamclient.so` (desktop client) | 2026-11-12 | `Wait` skips fds of 1024 or more. This build has no `__fdelt_chk` call in `Wait`. |
| `linux64/steamclient.so` (steamcmd, SDK, dedicated servers) | 2026-11-12 | Unchanged. Both triggers still abort. |

`CreateAsyncSocket` still lists the socket before calling `socket()` in both binaries.

## License

GPL-3.0. See `LICENSE`.
