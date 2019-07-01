#!/usr/bin/env python3
# Copyright (c) 2017 The Particl Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from test_framework.test_particl import ParticlTestFramework
from test_framework.test_particl import isclose
from test_framework.util import *

class AnonTest(ParticlTestFramework):

    def __init__(self):
        super().__init__()
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.extra_args = [ ['-debug',] for i in range(self.num_nodes)]

    def setup_network(self, split=False):
        self.nodes = self.start_nodes(self.num_nodes, self.options.tmpdir, self.extra_args)
        
        connect_nodes_bi(self.nodes, 0, 1)
        connect_nodes_bi(self.nodes, 0, 2)
        self.is_network_split = False
        self.sync_all()

    def run_test(self):
        nodes = self.nodes
        
        # Stop staking
        ro = nodes[0].reservebalance(True, 10000000)
        ro = nodes[1].reservebalance(True, 10000000)
        
        ro = nodes[0].extkeyimportmaster("abandon baby cabbage dad eager fabric gadget habit ice kangaroo lab absorb")
        assert(ro['account_id'] == 'aaaZf2qnNr5T7PWRmqgmusuu5ACnBcX2ev')
        
        ro = nodes[0].getinfo()
        assert(ro['total_balance'] == 100000)
        txnHashes = []

        ro = nodes[1].extkeyimportmaster("drip fog service village program equip minute dentist series hawk crop sphere olympic lazy garbage segment fox library good alley steak jazz force inmate")
        sxAddrTo1_1 = nodes[1].getnewstealthaddress()
        assert(sxAddrTo1_1 == 'TetbYTGv5LiqyFiUD3a5HHbpSinQ9KiRYDGAMvRzPfz4RnHMbKGAwDr1fjLGJ5Eqg1XDwpeGyqWMiwdK3qM3zKWjzHNpaatdoHVzzA')
        
        sxAddrTo0_1 = nodes[0].getnewstealthaddress()
        
        
        txnHash = nodes[0].sendparttoanon(sxAddrTo1_1, 1, '', '', False, 'node0 -> node1 p->a')
        txnHashes.append(txnHash)
        
        txnHash = nodes[0].sendparttoblind(sxAddrTo0_1, 1000, '', '', False, 'node0 -> node0 p->b')
        txnHashes.append(txnHash)
        
        txnHash = nodes[0].sendblindtoanon(sxAddrTo1_1, 100, '', '', False, 'node0 -> node1 b->a 1')
        txnHashes.append(txnHash)
        
        txnHash = nodes[0].sendblindtoanon(sxAddrTo1_1, 100, '', '', False, 'node0 -> node1 b->a 2')
        txnHashes.append(txnHash)
        
        txnHash = nodes[0].sendblindtoanon(sxAddrTo1_1, 100, '', '', False, 'node0 -> node1 b->a 3')
        txnHashes.append(txnHash)
        
        txnHash = nodes[0].sendblindtoanon(sxAddrTo1_1, 100, '', '', False, 'node0 -> node1 b->a 4')
        txnHashes.append(txnHash)
        
        for k in range(6):
                txnHash = nodes[0].sendparttoanon(sxAddrTo1_1, 10, '', '', False, 'node0 -> node1 p->a')
                print("txnHash ", txnHash)
                txnHashes.append(txnHash)
        
        assert(self.wait_for_mempool(nodes[1], txnHash))
        
        
        ro = nodes[1].listtransactions()
        #print("1 listtransactions ", json.dumps(ro, indent=4, default=self.jsonDecimal))
        
        ro = nodes[0].walletsettings('stakelimit', {'height':1})
        ro = nodes[0].reservebalance(False)
        
        assert(self.wait_for_height(nodes[1], 1))
        
        ro = nodes[0].reservebalance(True, 10000000)
        
        block1_hash = nodes[1].getblockhash(1)
        ro = nodes[1].getblock(block1_hash)
        for txnHash in txnHashes:
            assert(txnHash in ro['tx'])
        
        
        txnHash = nodes[1].sendanontoanon(sxAddrTo0_1, 1, '', '', False, 'node1 -> node0 a->a')
        print("1 sendanontoanon ", json.dumps(txnHash, indent=4, default=self.jsonDecimal))
        txnHashes = [txnHash,]
        
        assert(self.wait_for_mempool(nodes[0], txnHash))
        
        ro = nodes[0].listtransactions()
        #print("0 listtransactions ", json.dumps(ro, indent=4, default=self.jsonDecimal))
        
        ro = nodes[0].walletsettings('stakelimit', {'height':2})
        ro = nodes[0].reservebalance(False)
        
        assert(self.wait_for_height(nodes[1], 2))
        
        ro = nodes[0].reservebalance(True, 10000000)
        
        block1_hash = nodes[1].getblockhash(2)
        ro = nodes[1].getblock(block1_hash)
        for txnHash in txnHashes:
            assert(txnHash in ro['tx'])
        
        ro = nodes[1].anonoutput()
        print(ro)
        
        txnHash = nodes[1].sendanontoanon(sxAddrTo0_1, 101, '', '', False, 'node1 -> node0 a->a', 5, 1)
        print("1 sendanontoanon ", json.dumps(txnHash, indent=4, default=self.jsonDecimal))
        txnHashes = [txnHash,]
        
        assert(self.wait_for_mempool(nodes[0], txnHash))
        
        txnHash = nodes[1].sendanontoanon(sxAddrTo0_1, 0.1, '', '', False, '', 5, 2)
        print("1 sendanontoanon ", json.dumps(txnHash, indent=4, default=self.jsonDecimal))
        txnHashes = [txnHash,]
        
        assert(self.wait_for_mempool(nodes[0], txnHash))
        
        #assert(False)
        #print(json.dumps(ro, indent=4, default=self.jsonDecimal))

if __name__ == '__main__':
    AnonTest().main()
