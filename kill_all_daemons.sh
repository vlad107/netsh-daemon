#!/bin/bash
for x in $(pidof ./netsh); do kill $x; done
