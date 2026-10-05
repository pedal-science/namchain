# namchainctl against a real namchaind, over both transports.
#
#   cmake -DNAMCHAIND=... -DNAMCHAINCTL=... -DNAMCHAIN_WORK=... -DNAMCHAIN_MODELS=... \
#         -P tests/cli/run_ctl.cmake
#
# This is the only test that runs the daemon as a *process* and talks to it with the reference client,
# and that is the point of it: everything in tests/test_tcp.cpp exercises the transport in-process,
# where a mistake in the client's idea of the protocol cannot show up. It did: the first version of
# namchainctl sent `{"method":"hello"}` because the command name was used as the method, and the daemon
# answered, correctly, that there is no such method. Two separately correct halves do not make a
# working protocol, so the two halves are joined here.
#
# It also checks the parts of the transport that are only interesting in a real process: the token
# file, the port the kernel picked (read back from engine.status over the *other* transport), a
# refusal a client can act on, and the daemon going away when it is asked to.
foreach(_required NAMCHAIND NAMCHAINCTL NAMCHAIN_WORK NAMCHAIN_MODELS)
  if(NOT DEFINED ${_required})
    message(FATAL_ERROR "run_ctl.cmake: ${_required} is required")
  endif()
endforeach()

set(_work "${NAMCHAIN_WORK}/namchainctl")
set(_socket "${_work}/namchaind.sock")
set(_token "0123456789abcdef0123456789abcdef")
file(REMOVE_RECURSE "${_work}")
file(MAKE_DIRECTORY "${_work}/rigs")
file(WRITE "${_work}/rigs/gain.json" [=[
{
  "name": "Gain",
  "nodes": [
    { "id": "in", "type": "nc.input" },
    { "id": "g", "type": "nc.gain", "gain_db": -6 },
    { "id": "out", "type": "nc.output" }
  ],
  "connections": [ { "from": "in", "to": "g" }, { "from": "g", "to": "out" } ]
}
]=])
file(WRITE "${_work}/rigs/two-chains.json" [=[
{
  "name": "Guitar + Vocal",
  "chains": [
    {
      "id": "guitar", "name": "Guitar", "input_channel": 0, "output_channel": 0,
      "nodes": [
        { "id": "in", "type": "nc.input" },
        { "id": "g", "type": "nc.gain", "gain_db": -3 },
        { "id": "out", "type": "nc.output" }
      ],
      "connections": [ { "from": "in", "to": "g" }, { "from": "g", "to": "out" } ]
    },
    {
      "id": "vocal", "name": "Vocal", "input_channel": 1, "output_channel": 1,
      "nodes": [
        { "id": "in", "type": "nc.input" },
        { "id": "trim", "type": "nc.gain", "gain_db": -12 },
        { "id": "out", "type": "nc.output" }
      ],
      "connections": [ { "from": "in", "to": "trim" }, { "from": "trim", "to": "out" } ]
    }
  ]
}
]=])
file(WRITE "${_work}/token" "${_token}\n")
file(CHMOD "${_work}/token" PERMISSIONS OWNER_READ OWNER_WRITE)
file(WRITE "${_work}/ops.json" [=[ [ { "op": "set_param", "node": "g", "param": "gain_db", "value": -9 } ] ]=])

# A daemon in the background, with both transports up and a TCP port the kernel picks -- read back
# from engine.status below rather than fixed here, so two runs of this test cannot collide.
execute_process(
  COMMAND sh -c "exec \"${NAMCHAIND}\" --backend null --no-lock-memory --rt-priority 0 \
      --capture-channels 2 --playback-channels 2 \
      --socket \"${_socket}\" --rigs-root \"${_work}/rigs\" --rig gain.json \
      --models-root \"${NAMCHAIN_MODELS}\" --irs-root \"${NAMCHAIN_MODELS}\" \
      --tcp-listen --tcp-address 127.0.0.1 --tcp-port 0 --tcp-token-file \"${_work}/token\" \
      > \"${_work}/daemon.log\" 2>&1 & echo $!"
  OUTPUT_VARIABLE _daemon_pid
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _daemon_pid MATCHES "^[0-9]+$")
  message(FATAL_ERROR "run_ctl.cmake: could not start namchaind (got '${_daemon_pid}')")
endif()

function(_kill_daemon)
  execute_process(COMMAND kill -TERM ${_daemon_pid} ERROR_QUIET)
  # Give it a moment to unlink its socket, then make sure it is gone whatever it decided.
  execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 0.3)
  execute_process(COMMAND kill -KILL ${_daemon_pid} ERROR_QUIET)
endfunction()

macro(_fail _message)
  file(READ "${_work}/daemon.log" _log)
  _kill_daemon()
  message(FATAL_ERROR "${_message}\n--- daemon log ---\n${_log}")
endmacro()

# The daemon binds the socket before it starts the render thread, so waiting for the file is waiting
# for the control loop to be reachable.
set(_ready FALSE)
foreach(_attempt RANGE 100)
  if(EXISTS "${_socket}")
    set(_ready TRUE)
    break()
  endif()
  execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 0.05)
endforeach()
if(NOT _ready)
  _fail("the control socket never appeared")
endif()

# --- the same protocol over either transport ---------------------------------
execute_process(COMMAND "${NAMCHAINCTL}" --socket "${_socket}" hello --compact
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _hello ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _hello MATCHES "\"ok\":true")
  _fail("`hello` over the Unix socket failed (${_rc}): ${_hello} ${_err}")
endif()
if(NOT _hello MATCHES "\"transport\":\"unix\"")
  _fail("`hello` over the Unix socket did not report its transport: ${_hello}")
endif()

execute_process(COMMAND "${NAMCHAINCTL}" --socket "${_socket}" status --compact
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _status ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  _fail("`status` over the Unix socket failed (${_rc}): ${_err}")
endif()

# The meter's method, over the reference client: it must answer with its own object, and the object
# must have the shape a meter needs (a peak per device channel and a chain whose nodes carry their
# client-side identity), not merely parse.
execute_process(COMMAND "${NAMCHAINCTL}" --socket "${_socket}" levels --compact
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _levels ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _levels MATCHES "\"ok\":true")
  _fail("`levels` over the Unix socket failed (${_rc}): ${_levels} ${_err}")
endif()
if(_levels MATCHES "\"status\"")
  _fail("`levels` answered with a status object rather than its own: ${_levels}")
endif()
string(JSON _state ERROR_VARIABLE _levels_error GET "${_levels}" levels state)
if(_levels_error)
  _fail("`levels` has no levels object: ${_levels}")
endif()
if(NOT _state STREQUAL "running" AND NOT _state STREQUAL "closed" AND NOT _state STREQUAL "lost")
  _fail("`levels` reported a state that is not one of the three: ${_state}")
endif()
string(JSON _peak GET "${_levels}" levels input peak)
string(JSON _peak0 GET "${_levels}" levels input peak 0)
if(NOT _peak0 MATCHES "^-?[0-9]+\.[0-9]+$")
  _fail("a device channel's peak is not a dBFS number: ${_peak}")
endif()
string(JSON _node_element GET "${_levels}" levels chains 0 nodes 0 element)
if(NOT _node_element STREQUAL "g" AND NOT _node_element STREQUAL "in" AND NOT _node_element STREQUAL "out")
  _fail("a node's level carries no client-side identity: ${_levels}")
endif()
string(JSON _port GET "${_status}" status control tcp_port)
string(JSON _listening GET "${_status}" status control tcp_listening)
if(NOT _listening OR _port EQUAL 0)
  _fail("the daemon reports no TCP listener: ${_status}")
endif()

execute_process(COMMAND "${NAMCHAINCTL}" --host 127.0.0.1 --port ${_port}
                        --token-file "${_work}/token" hello --compact
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _hello ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _hello MATCHES "\"transport\":\"tcp\"")
  _fail("`hello` over TCP failed (${_rc}): ${_hello} ${_err}")
endif()
if(NOT _hello MATCHES "\"authenticated\":true")
  _fail("the TCP session did not authenticate: ${_hello}")
endif()

# --- a refusal a client can act on -------------------------------------------
execute_process(COMMAND "${NAMCHAINCTL}" --host 127.0.0.1 --port ${_port}
                        --token deadbeefdeadbeefdeadbeefdeadbeef status
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(_rc EQUAL 0 OR NOT _err MATCHES "unauthorized")
  _fail("a wrong token was not refused as unauthorized (exit ${_rc}): ${_err}")
endif()

execute_process(COMMAND "${NAMCHAINCTL}" --host 127.0.0.1 --port ${_port} status
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(_rc EQUAL 0 OR NOT _err MATCHES "requires --token")
  _fail("a TCP command without a token was not refused by the client (exit ${_rc}): ${_err}")
endif()

# --- the commands a UI would use ---------------------------------------------
execute_process(COMMAND "${NAMCHAINCTL}" --host 127.0.0.1 --port ${_port}
                        --token-file "${_work}/token" status --compact
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _status ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  _fail("`status` over TCP failed (${_rc}): ${_err}")
endif()
string(JSON _health GET "${_status}" status health)
if(NOT _health STREQUAL "ok")
  _fail("the daemon is not healthy: ${_status}")
endif()

# A parameter edit that ramps, which is the answer that tells a UI it may stream this control.
execute_process(COMMAND "${NAMCHAINCTL}" --socket "${_socket}" set --node g --param gain_db --value -6 --compact
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "\"kind\":\"smooth\"")
  _fail("`set` on a rampable parameter was not smooth (${_rc}): ${_out} ${_err}")
endif()

# A patch from a file, which is how a UI's structural edit arrives.
execute_process(COMMAND "${NAMCHAINCTL}" --socket "${_socket}" patch "${_work}/ops.json" --compact
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "\"applied\":1")
  _fail("`patch` from a file failed (${_rc}): ${_out} ${_err}")
endif()

# A document with two chains, applied over TCP, then read back: the chain ids are the client's and
# they survive the round trip.
execute_process(COMMAND "${NAMCHAINCTL}" --host 127.0.0.1 --port ${_port} --token-file "${_work}/token"
                        apply "${_work}/rigs/two-chains.json" --compact
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "\"chains\":2")
  _fail("`apply` of a two-chain document failed (${_rc}): ${_out} ${_err}")
endif()

execute_process(COMMAND "${NAMCHAINCTL}" --socket "${_socket}" get --compact
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "guitar" OR NOT _out MATCHES "vocal")
  _fail("`get` did not read back both chains (${_rc}): ${_out} ${_err}")
endif()

# One chain muted by name, and the other left alone: the answer says which.
execute_process(COMMAND "${NAMCHAINCTL}" --socket "${_socket}" --chain vocal mute on --compact
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "\"chain\":\"vocal\"")
  _fail("`mute` of one chain failed (${_rc}): ${_out} ${_err}")
endif()

execute_process(COMMAND "${NAMCHAINCTL}" --socket "${_socket}" mute off --compact
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "\"mute\":false")
  _fail("`mute off` failed (${_rc}): ${_out} ${_err}")
endif()

execute_process(COMMAND "${NAMCHAINCTL}" --socket "${_socket}" schema --compact
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "nc.gain")
  _fail("`schema` failed (${_rc}): ${_err}")
endif()

# The library, and the one thing that makes it worth having: a document names an amp by id, and the
# client never learns where the models live. `assets` is also where a client gets those ids.
execute_process(COMMAND "${NAMCHAINCTL}" --socket "${_socket}" assets --compact
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "A2-Lite")
  _fail("`assets` did not list the library (${_rc}): ${_out} ${_err}")
endif()
file(WRITE "${_work}/by-id.json" [=[
{
  "name": "by id",
  "nodes": [
    { "id": "in", "type": "nc.input" },
    { "id": "amp", "type": "nc.nam", "model": "A2-Lite" },
    { "id": "out", "type": "nc.output" }
  ],
  "connections": [ { "from": "in", "to": "amp" }, { "from": "amp", "to": "out" } ]
}
]=])
execute_process(COMMAND "${NAMCHAINCTL}" --host 127.0.0.1 --port ${_port} --token-file "${_work}/token"
                        apply "${_work}/by-id.json" --compact
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "\"ok\":true")
  _fail("`apply` of a document naming a model by id failed (${_rc}): ${_out} ${_err}")
endif()

# --- the daemon stops when it is asked to ------------------------------------
_kill_daemon()
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 0.2)
if(EXISTS "${_socket}")
  _fail("the daemon left its socket behind")
endif()
