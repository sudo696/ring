
#!/usr/bin/env python3
from test_framework.test_framework import RingTestFramework
from test_framework.util import assert_equal

class RNGDWCFeatureTest(RingTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def run_test(self):
        # Test 1: Verify RNG total supply
        # Generate enough blocks to verify max supply
        blocks = self.nodes[0].generate(1000)
        supply = self.nodes[0].gettxoutsetinfo()['total_amount']
        assert supply <= 9000000, f"Total supply exceeds 9 million: {supply}"

        # Test 2: Verify block reward
        for i in range(10):
            blocks = self.nodes[0].generate(5)
            reward = self.nodes[0].getblock(blocks[-1])['reward']
            assert_equal(reward, 1, "Block reward should be 1 RNG every 5 blocks")

        # Test 3 & 4: DWC burn mechanism
        burn_tx = self.nodes[0].burndwc(1.0)  # Minimum burn amount
        assert len(self.nodes[0].getrawmempool()) > 0, "Burn transaction not in mempool"
        
        # Generate 5 blocks for confirmation
        self.nodes[0].generate(5)
        tx = self.nodes[0].gettransaction(burn_tx)
        assert tx['confirmations'] >= 5, "Burn transaction didn't get required confirmations"

        # Test 5 & 6: Vote weight and random selection
        vote_weight = self.nodes[0].getvoteweight(burn_tx)
        expected_weight = 100  # 1 DWC = 100 votes (0.01 DWC per vote)
        assert_equal(vote_weight, expected_weight, "Vote weight calculation incorrect")

if __name__ == '__main__':
    RNGDWCFeatureTest().main()
