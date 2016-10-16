#!/bin/bash

let port=`cat port`
(echo "cat"; echo "stdin") | socat STDIO TCP:localhost:$port
(echo -n "l"; sleep 0.8; echo "s") | socat STDIO TCP:localhost:$port &
(echo -n "ca"; sleep 0.4; echo "t"; echo "123") | socat STDIO TCP:localhost:$port &
sleep 1
(echo -n "ca"; sleep 1.0; echo "t"; echo "thread 1") | socat STDIO TCP:localhost:$port &
(echo -n "ca"; sleep 0.5; echo "t"; echo "thread 2") | socat STDIO TCP:localhost:$port &
(echo -n "ca"; sleep 0.7; echo "t"; echo "thread 3") | socat STDIO TCP:localhost:$port &
(echo "cat"; sleep 1.5; echo "thread 4") | socat STDIO TCP:localhost:$port &
