+-----------------------------------+------------------------------------------------------------------------+
| Command                           | Description                                                            |
+===================================+========================================================================+
| **stream-stats**                  | | Display stream traffic statistics.                                   |
+-----------------------------------+------------------------------------------------------------------------+
| **stream-info**                   | | Display stream/flow information.                                     |
|                                   | |                                                                      |
|                                   | | **Arguments:**                                                       |
|                                   | | ``flow-id``                                                          |
+-----------------------------------+------------------------------------------------------------------------+
| **stream-summary**                | | Display stream/flow summary information.                             |
|                                   | |                                                                      |
|                                   | | **Arguments:**                                                       |
|                                   | | ``session-group-id``                                                 |
|                                   | | ``name`` stream name                                                 |
|                                   | | ``interface`` TX interface name                                      |
|                                   | | ``direction`` [both(default), upstream, downstream]                  |
+-----------------------------------+------------------------------------------------------------------------+
| **stream-reset**                  | | Reset all traffic streams.                                           |
+-----------------------------------+------------------------------------------------------------------------+
| **stream-start**                  | | This command can be used to start or stop traffic stream flows.      |
|                                   | | This command applies to all flows except session-traffic and         |
| **stream-stop**                   | | multicast. If you provide a specific ``flow-id`` as an argument,     |
|                                   | | other arguments are ignored. In this particular case, you can also   |
| **stream-stop-verified**          | | start and stop session-traffic and multicast.                        |
|                                   | |                                                                      |
|                                   | | The command **stream-stop-verified** works similar to                |
|                                   | | **stream-stop** but only verified streams will be stopped.           |
|                                   | |                                                                      |
|                                   | | **Arguments:**                                                       |
|                                   | | ``flow-id``                                                          |
|                                   | | ``session-id``                                                       |
|                                   | | ``session-group-id`` (ignored if session-id is present)              |
|                                   | | ``name`` stream name                                                 |
|                                   | | ``interface`` TX interface name                                      |
|                                   | | ``direction`` [both(default), upstream, downstream]                  |
+-----------------------------------+------------------------------------------------------------------------+
| **streams-pending**               | | List flow-id of all pending (not verified) traffic streams.          |
+-----------------------------------+------------------------------------------------------------------------+
| **stream-update**                 | | Update stream/flow configuration.                                    |
|                                   | |                                                                      |
|                                   | | **Arguments:**                                                       |
|                                   | | ``flow-id``                                                          |
|                                   | | ``tcp-flags`` [ack, fin, fin-ack, syn, syn-ack, rst, push, push-ack] |
|                                   | | ``pps``                                                              |
+-----------------------------------+------------------------------------------------------------------------+
