#!/bin/bash
cd WTP-opt
make clean
make
sudo python ../topology.py
