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

#include "sib_pdu_assembler.h"
#include "srsran/srslog/srslog.h"
#include "srsran/support/units.h"

using namespace srsran;

// Max SI Message PDU size. This value is implementation-defined.
static constexpr unsigned MAX_BCCH_DL_SCH_PDU_SIZE = 2048;

// Payload of zeros sent to when an error occurs.
static const std::vector<uint8_t> zeros_payload(MAX_BCCH_DL_SCH_PDU_SIZE, 0);

sib_pdu_assembler::sib_pdu_assembler(const mac_cell_sys_info_config& req) : logger(srslog::fetch_basic_logger("MAC"))
{
  // Version starts at 0.
  last_cfg_buffers.version  = 0;
  last_cfg_buffers.sib1_len = units::bytes{0};
  save_buffers(0, req);
  current_buffers = last_cfg_buffers;
}

void sib_pdu_assembler::handle_si_change_request(const mac_cell_sys_info_config& req)
{
  // Save new buffers that have changed.
  srsran_assert(last_cfg_buffers.version != req.si_sched_cfg.version,
                "Version of the last SI message update is the same as the new one");
  save_buffers(req.si_sched_cfg.version, req);

  // Forward new version and buffers to SIB assembler RT path.
  pending.write_and_commit(last_cfg_buffers);
}

static std::shared_ptr<const std::vector<uint8_t>> make_linear_buffer(const byte_buffer& pdu)
{
  // Note: We overallocate the SI message buffer to account for padding.
  // Note: After this point, resizing the vector is not possible as it would invalidate the spans passed to lower
  // layers.
  auto vec = std::make_shared<std::vector<uint8_t>>(MAX_BCCH_DL_SCH_PDU_SIZE, 0);
  copy_segments(pdu, span<uint8_t>(vec->data(), vec->size()));
  return vec;
}

void sib_pdu_assembler::save_buffers(si_version_type si_version, const mac_cell_sys_info_config& req)
{
  // Note: In case the SIB1/SI message does not change, the comparison between the respective byte_buffers should be
  // fast (as they will point to the same memory location). Avoid at all costs the operator== for the stored vectors
  // as they are overdimensioned to account for padding.

  // Check if SIB1 has changed.
  if (last_si_cfg.sib1 != req.sib1) {
    last_si_cfg.sib1             = req.sib1.copy();
    last_cfg_buffers.sib1_len    = units::bytes{static_cast<unsigned>(req.sib1.length())};
    last_cfg_buffers.sib1_buffer = make_linear_buffer(req.sib1);
  }

  // Check if SI messages have changed.
  last_cfg_buffers.si_msg_buffers.resize(req.si_messages.size());
  for (unsigned i = 0, e = req.si_messages.size(); i != e; ++i) {
    if (last_si_cfg.si_messages.size() <= i) {
      last_si_cfg.si_messages.resize(i + 1);
    }
    if (req.si_messages[i] != last_si_cfg.si_messages[i]) {
      last_si_cfg.si_messages[i]                = req.si_messages[i].copy();
      last_cfg_buffers.si_msg_buffers[i].first  = units::bytes{static_cast<unsigned>(req.si_messages[i].length())};
      last_cfg_buffers.si_msg_buffers[i].second = make_linear_buffer(req.si_messages[i]);
    }
  }

  last_cfg_buffers.version = si_version;
}

span<const uint8_t> sib_pdu_assembler::encode_si_pdu(slot_point sl_tx, const sib_information& si_info)
{
  const unsigned tbs = si_info.pdsch_cfg.codewords[0].tb_size_bytes;
  srsran_assert(tbs <= MAX_BCCH_DL_SCH_PDU_SIZE, "BCCH-DL-SCH is too long. Revisit constant");

  if (si_info.version != current_buffers.version) {
    // Current SI message version is too old. Fetch new version from shared buffer.
    current_buffers = pending.read();
    if (current_buffers.version != si_info.version) {
      // Versions do not match.
      logger.error("SI message version mismatch. Expected: {}, got: {}", si_info.version, current_buffers.version);
      // We force the version to avoid more than one warning.
      current_buffers.version = si_info.version;
    }
  }

  if (si_info.si_indicator == sib_information::si_indicator_type::sib1) {
    if (current_buffers.sib1_len.value() > tbs) {
      logger.warning("Failed to encode SIB1 PDSCH. Cause: PDSCH TB size {} is smaller than the SIB1 length {}",
                     tbs,
                     current_buffers.sib1_len);
      return span<const uint8_t>{zeros_payload}.first(tbs);
    }
    return span<const uint8_t>(current_buffers.sib1_buffer->data(), tbs);
  }

  srsran_assert(si_info.si_msg_index.has_value(), "Invalid SI message index");
  const unsigned idx = si_info.si_msg_index.value();
  if (idx >= current_buffers.si_msg_buffers.size()) {
    logger.error("Failed to encode SI-message in PDSCH. Cause: SI message index {} does not exist", idx);
    return span<const uint8_t>{zeros_payload}.first(tbs);
  }

  if (current_buffers.si_msg_buffers[idx].first.value() > tbs) {
    logger.warning(
        "Failed to encode SI-message {} PDSCH. Cause: PDSCH TB size {} is smaller than the SI-message length {}",
        idx,
        tbs,
        current_buffers.si_msg_buffers[idx].first.value());
    return span<const uint8_t>{zeros_payload}.first(tbs);
  }

  return span<const uint8_t>(current_buffers.si_msg_buffers[idx].second->data(), tbs);
}
