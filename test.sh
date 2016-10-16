#!/bin/bash

let port=`cat port`

sleep 0.1
echo 'ls' | socat STDIO TCP:localhost:$port
echo "echo -e 123 45 67 \n 123 456 67" | socat STDIO TCP:localhost:$port
echo "echo 1 | echo 1" | socat STDIO TCP:localhost:$port
echo "echo -e 123 45 67 \r\n 123 456 67\r\n | grep 56" | socat STDIO TCP:localhost:$port 
echo "cat /proc/cpuinfo | grep 'model name' | sed -re 's/.*: (.*)/\1/' | uniq" | socat STDIO TCP:localhost:$port
(echo "sh"; echo "echo \"hello, I am shell\"") | socat STDIO TCP:localhost:$port
(echo "sh"; echo "echo \"hello, I am shell\""; sleep 0.5; echo "echo \"I am still here\"") | socat STDIO TCP:localhost:$port
(echo "sh"; echo "cat"; echo "I am cat, meow"; sleep 0.1) | socat STDIO TCP:localhost:$port
(echo "sh"; echo "cat"; sleep 0.1; echo "I am cat, meow"; sleep 0.1) | socat STDIO TCP:localhost:$port
(echo "ls"; sleep 1.0; echo "ls") | nc localhost $port
(echo "echo \"echo \"Inception\"\" | socat STDIO TCP:localhost:$port") | socat STDIO TCP:localhost:$port


