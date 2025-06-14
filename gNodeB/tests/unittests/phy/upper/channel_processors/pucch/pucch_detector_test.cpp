/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

/// \file
/// \brief PUCCH detector unit test.
///
/// The test reads received symbols and channel coefficients from a test vector, detects a PUCCH Format 1 transmission
/// and compares the resulting bits (SR or HARQ-ACK) with the expected ones.

#include "pucch_detector_test_data.h"
#include "srsran/phy/generic_functions/generic_functions_factories.h"
#include "srsran/phy/upper/channel_processors/channel_processor_factories.h"
#include "srsran/phy/upper/channel_processors/pucch/factories.h"
#include "srsran/phy/upper/equalization/equalization_factories.h"
#include <gtest/gtest.h>

using namespace srsran;

/// \cond
namespace srsran {

std::ostream& operator<<(std::ostream& os, const test_case_t& tc)
{
  std::string hops = (tc.cfg.second_hop_prb.has_value() ? "intraslot frequency hopping" : "no frequency hopping");
  return os << fmt::format(
             "Numerology {}, {} port(s), {}, symbol allocation [{}, {}], {} HARQ-ACK bit(s), {} SR bit(s).",
             tc.cfg.slot.numerology(),
             tc.cfg.ports.size(),
             hops,
             tc.cfg.start_symbol_index,
             tc.cfg.nof_symbols,
             tc.cfg.nof_harq_ack,
             tc.sr_bit.size());
}

} // namespace srsran

namespace {

class PUCCHDetectFixture : public ::testing::TestWithParam<test_case_t>
{
protected:
  static void SetUpTestSuite()
  {
    if (!detector_test) {
      std::shared_ptr<low_papr_sequence_generator_factory> low_papr_gen =
          create_low_papr_sequence_generator_sw_factory();
      std::shared_ptr<low_papr_sequence_collection_factory> low_papr_col =
          create_low_papr_sequence_collection_sw_factory(low_papr_gen);
      std::shared_ptr<pseudo_random_generator_factory> pseudorandom = create_pseudo_random_generator_sw_factory();
      std::shared_ptr<channel_equalizer_factory>       equalizer    = create_channel_equalizer_generic_factory();
      std::shared_ptr<dft_processor_factory>           dft          = create_dft_processor_factory_fftw_slow();
      std::shared_ptr<pucch_detector_factory>          detector_factory =
          create_pucch_detector_factory_sw(low_papr_col, pseudorandom, equalizer, dft);
      report_fatal_error_if_not(detector_factory, "Failed to create factory.");

      detector_test = detector_factory->create();
      report_fatal_error_if_not(detector_test, "Failed to create detector.");
      ASSERT_NE(detector_test, nullptr);

      channel_estimate::channel_estimate_dimensions ch_dims;
      ch_dims.nof_tx_layers = 1;
      ch_dims.nof_rx_ports  = MAX_PORTS;
      ch_dims.nof_symbols   = MAX_NSYMB_PER_SLOT;
      ch_dims.nof_prb       = MAX_RB;
      csi.resize(ch_dims);
    }
  }

  static void TearDownTestSuite() { detector_test.reset(); }

  static std::unique_ptr<pucch_detector> detector_test;
  static channel_estimate                csi;
};

std::unique_ptr<pucch_detector> PUCCHDetectFixture::detector_test = nullptr;
channel_estimate                PUCCHDetectFixture::csi;

void fill_ch_estimate(channel_estimate& ch_est, const std::vector<resource_grid_reader_spy::expected_entry_t>& entries)
{
  for (const auto& entry : entries) {
    ch_est.set_ch_estimate(entry.value, entry.subcarrier, entry.symbol, entry.port);
  }
}

TEST_P(PUCCHDetectFixture, Format1Test)
{
  test_case_t test_data = GetParam();

  pucch_detector::format1_configuration config = test_data.cfg;

  unsigned                                                nof_res      = config.nof_symbols / 2 * NRE;
  unsigned                                                nof_ports    = config.ports.size();
  std::vector<resource_grid_reader_spy::expected_entry_t> grid_entries = test_data.received_symbols.read();
  ASSERT_EQ(grid_entries.size(), nof_res * nof_ports)
      << "The number of grid entries and the number of PUCCH REs do not match";

  resource_grid_reader_spy grid(0, 0, 0);

  grid.write(grid_entries);

  std::vector<resource_grid_reader_spy::expected_entry_t> channel_entries = test_data.ch_estimates.read();
  ASSERT_EQ(channel_entries.size(), nof_res * nof_ports)
      << "The number of channel estimates and the number of PUCCH REs do not match";

  fill_ch_estimate(csi, channel_entries);

  for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
    csi.set_noise_variance(test_data.noise_var, i_port);
  }

  pucch_detector::pucch_detection_result res = detector_test->detect(grid, csi, test_data.cfg);

  pucch_uci_message& msg = res.uci_message;

  if (test_data.cfg.nof_harq_ack == 0) {
    if (test_data.sr_bit.empty()) {
      // The second part of the condition is to accept false detection if the detection metric is just above the
      // threshold. False alarm probability has to be evaluated in a dedicated test.
      ASSERT_TRUE((msg.get_status() == uci_status::invalid) || (res.detection_metric < 1.3))
          << "An empty PUCCH occasion should return an 'invalid' UCI.";
      return;
    }
    if (test_data.sr_bit[0] == 1) {
      ASSERT_EQ(msg.get_status(), uci_status::valid)
          << "A positive SR-only PUCCH occasion should return a 'valid' UCI.";
      return;
    }
    ASSERT_EQ(msg.get_status(), uci_status::invalid)
        << "A negative SR-only PUCCH occasion should return an 'invalid' UCI.";
    return;
  }

  ASSERT_EQ(msg.get_status(), uci_status::valid);

  ASSERT_EQ(msg.get_harq_ack_bits().size(), test_data.ack_bits.size()) << "Wrong number of HARQ-ACK bits.";
  ASSERT_TRUE(std::equal(msg.get_harq_ack_bits().begin(), msg.get_harq_ack_bits().end(), test_data.ack_bits.begin()))
      << "The HARQ-ACK bits do not match.";
}

// This test checks the behavior of the detector when the estimated noise variance is zero.
TEST_P(PUCCHDetectFixture, Format1Variance0Test)
{
  test_case_t test_data = GetParam();

  pucch_detector::format1_configuration config = test_data.cfg;

  unsigned                                                nof_res      = config.nof_symbols / 2 * NRE;
  unsigned                                                nof_ports    = config.ports.size();
  std::vector<resource_grid_reader_spy::expected_entry_t> grid_entries = test_data.received_symbols.read();
  ASSERT_EQ(grid_entries.size(), nof_res * nof_ports)
      << "The number of grid entries and the number of PUCCH REs do not match";

  resource_grid_reader_spy grid(0, 0, 0);

  grid.write(grid_entries);

  std::vector<resource_grid_reader_spy::expected_entry_t> channel_entries = test_data.ch_estimates.read();
  ASSERT_EQ(channel_entries.size(), nof_res * nof_ports)
      << "The number of channel estimates and the number of PUCCH REs do not match";

  fill_ch_estimate(csi, channel_entries);

  for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
    csi.set_noise_variance(0, i_port);
  }

  pucch_detector::pucch_detection_result res = detector_test->detect(grid, csi, test_data.cfg);
  pucch_uci_message&                     msg = res.uci_message;
  ASSERT_EQ(msg.get_status(), uci_status::invalid)
      << "When the signal is ill-conditioned, the detection status should be invalid.";
}

INSTANTIATE_TEST_SUITE_P(PUCCHDetectorSuite, PUCCHDetectFixture, ::testing::ValuesIn(pucch_detector_test_data));
} // namespace

/// \endcond
