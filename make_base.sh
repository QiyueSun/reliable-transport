#!/bin/bash
cd WTP-base
make clean
make
sudo python ../topology.py
