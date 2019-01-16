#!/usr/bin/env python3
import numpy as np
import sys
import torch

net  = torch.load(sys.argv[1])
state = net['state_dict']

t=state
name = 'init_conv'

# weight = np.array(state['init_conv.0.weight'])
# print(weight)


weight = np.array(t[name + '.0.weight'])    
# bias = np.array(t[name + '.0.bias'])
bn_gamma = np.array([name + '.1.weight'])
# bn_beta = np.array(t[name + '.1.bias'])
# bn_mean = np.array(t[name + '.1.running_mean'])
# bn_var = np.array(t[name + '.1.running_var'])

weight *= bn_gamma[:, np.newaxis, np.newaxis, np.newaxis]

print(weight)