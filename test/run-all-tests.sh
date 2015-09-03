#!/bin/bash

for x in *-test.rb ; do
    echo -n "$x "
    ./${x} $@ && echo ok
done
