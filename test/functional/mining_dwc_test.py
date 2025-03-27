
#!/usr/bin/env python3
# Copyright (c) 2023 The Ring Core developers
# Distributed under the MIT software license

from test_framework.test_framework import RingTestFramework
from test_framework.util import assert_equal
from test_framework.blocktools import create_coinbase
from test_framework.messages import CBlock, CBlockHeader

class DWCMiningTest(RingTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        node = self.nodes[0]
        
        # Test RandomX mining
        self.log.info("Testing RandomX mining...")
        
        # Get block template
        template = node.getblocktemplate({'rules': ['segwit']})
        
        # Create block
        block = create_block(int(template['previousblockhash'], 16),
                           create_coinbase(template['height']),
                           template['curtime'])
        
        # Solve block with RandomX
        block.solve()
        
        # Submit and verify
        ret = node.submitblock(hexdata=b2x(block.serialize()))
        assert_equal(ret, None)
        assert_equal(node.getbestblockhash(), block.hash)
        
        self.log.info("RandomX mining test passed")
        
        # Test basic mining info
        mining_info = node.getmininginfo()

    def test_randomx_config(self):
        """Test RandomX configuration"""
        node = self.nodes[0]
        
        mining_info = node.getmininginfo()
        assert_equal(mining_info['pow_algo'], 'randomx')
        
        # Verify RandomX parameters
        assert_equal(mining_info['randomx_init'], True)
        assert_equal(mining_info['randomx_mode'], 'fast') 

        assert_equal(mining_info['blocks'], 0)  # Should be 0 on clean chain
        assert mining_info['currentblocktx'] == 0
        assert mining_info['currentblockweight'] == 4000
        
        # Generate some blocks and verify RandomX algorithm
        address = node.getnewaddress()
        blocks = node.generatetoaddress(10, address)
        
        # Verify blocks were created
        assert_equal(len(blocks), 10)
        
        # Check block properties
        for block_hash in blocks:
            block = node.getblock(block_hash)
            # Verify RandomX parameters
            assert_equal(block['version'], 1)
            assert block['bits'] == "1e0fffff"  # Initial difficulty for RandomX
            assert int(block['nonce']) >= 0
            # Verify time is reasonable
            assert block['time'] > 0
            
        # Test block template generation
        template = node.getblocktemplate({'rules': ['segwit']})
        assert template['version'] == 1
        assert template['previousblockhash'] == blocks[-1]
        assert 'bits' in template
        assert 'height' in template
        assert template['height'] == 11  # After 10 blocks generated

        self.log.info("DWC mining tests passed")

if __name__ == '__main__':
    DWCMiningTest().main()
