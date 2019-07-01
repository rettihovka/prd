#!/usr/bin/env python3
# Copyright (c) 2017 The Particl Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from .test_framework import BitcoinTestFramework
from .util import *

import decimal

def isclose(a, b, rel_tol=1e-09, abs_tol=0.0):
    a = decimal.Decimal(a)
    b = decimal.Decimal(b)
    return abs(a-b) <= max(decimal.Decimal(rel_tol) * decimal.Decimal(max(abs(a), abs(b))), abs_tol)

class ParticlTestFramework(BitcoinTestFramework):
    def __init__(self):
        super().__init__()
    
    def start_node(self, i, dirname, extra_args=None, rpchost=None, timewait=None, binary=None, stderr=None):
        return super().start_node(i, dirname, extra_args, rpchost, timewait, binary, stderr, False)
    
    
    def wait_for_height(self, node, nHeight, nTries=500):
        for i in range(nTries):
            time.sleep(1)
            ro = node.getinfo()
            if ro['blocks'] >= nHeight:
                return True
        return False
    
    def wait_for_mempool(self, node, txnHash, nTries=50):
        for i in range(50):
            time.sleep(0.5)
            try:
                ro = node.getmempoolentry(txnHash)
                
                if ro['size'] >= 100 and ro['height'] >= 0:
                    return True
            except:
                continue
        return False
    
    def stakeToHeight(self, height, fSync=True, nStakeNode=0, nSyncCheckNode=1):
        ro = self.nodes[nStakeNode].walletsettings('stakelimit', {'height':height})
        ro = self.nodes[nStakeNode].reservebalance(False)
        assert(self.wait_for_height(self.nodes[nStakeNode], height))
        ro = self.nodes[nStakeNode].reservebalance(True, 10000000)
        if not fSync:
            return
        self.sync_all()
        assert(self.nodes[nSyncCheckNode].getblockcount() == height)
    
    def stakeBlocks(self, nBlocks):
        height = self.nodes[0].getblockcount()
        
        self.stakeToHeight(height + nBlocks)
    
    def jsonDecimal(self, obj):
        if isinstance(obj, decimal.Decimal):
            return str(obj)
        raise TypeError
