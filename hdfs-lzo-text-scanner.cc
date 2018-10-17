// Copyright (c) 2012 Cloudera, Inc. All rights reserved.
// This file is based on code from the lzop program which is:
//   Copyright (C) 1996-2010 Markus Franz Xaver Johannes Oberhumer
//   All Rights Reserved.
//
//   lzop and the LZO library are free software; you can redistribute them
//   and/or modify them under the terms of the GNU General Public License as
//   published by the Free Software Foundation; either version 2 of
//   the License, or (at your option) any later version.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License for more details.
//
//   You should have received a copy of the GNU General Public License
//   along with this program; see the file COPYING.
//   If not, write to the Free Software Foundation, Inc.,
//   59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.


#include "hdfs-lzo-text-scanner.h"

#include <hdfs.h>
#include <dlfcn.h>
#include <boost/algorithm/string.hpp>

#include "exec/hdfs-scan-node-base.h"
#include "exec/scanner-context.inline.h"
#include "runtime/runtime-state.h"
#include "runtime/hdfs-fs-cache.h"
#include "util/debug-util.h"
#include "util/error-util.h"
#include "util/hdfs-util.h"

#include "gen-cpp/Descriptors_types.h"

using namespace boost;
using namespace boost::algorithm;
using namespace impala;
using namespace std;

// Workaround to build against Impala before and after move from DiskIoMgr::ScanRange
// to io::ScanRange.
// TODO: remove this
#ifdef IMPALA_RUNTIME_IO_DISK_IO_MGR_H
using namespace impala::io;
#else
using ScanRange = DiskIoMgr::ScanRange;
#endif

DEFINE_bool(disable_lzo_checksums, true,
    "Disable internal checksum checking for Lzo compressed files, defaults true");

// The magic byte sequence at the beginning of an LZOP file.
static const uint8_t LZOP_MAGIC[9] =
    { 0x89, 0x4c, 0x5a, 0x4f, 0x00, 0x0d, 0x0a, 0x1a, 0x0a };

extern "C" HdfsLzoTextScanner* CreateLzoTextScanner(
    HdfsScanNodeBase* scan_node, RuntimeState* state) {
  return new HdfsLzoTextScanner(scan_node, state);
}

extern "C" Status LzoIssueInitialRangesImpl(HdfsScanNodeBase* scan_node,
    const vector<HdfsFileDesc*>& files) {
  return HdfsLzoTextScanner::LzoIssueInitialRangesImpl(scan_node, files);
}

// Macro to convert between ScannerContext errors to Status returns.
#define RETURN_IF_FALSE(x) if (UNLIKELY(!(x))) return status;

namespace impala {

HdfsLzoTextScanner::HdfsLzoTextScanner(HdfsScanNodeBase* scan_node, RuntimeState* state)
    : HdfsTextScanner(scan_node, state),
      block_buffer_pool_(new MemPool(scan_node->mem_tracker())),
      disable_checksum_(FLAGS_disable_lzo_checksums) {
}

HdfsLzoTextScanner::~HdfsLzoTextScanner() {
}

void HdfsLzoTextScanner::Close(RowBatch* row_batch) {
  if (row_batch != nullptr) {
    row_batch->tuple_data_pool()->AcquireData(block_buffer_pool_.get(), false);
  } else {
    block_buffer_pool_->FreeAll();
  }
  HdfsTextScanner::Close(row_batch);
}

Status HdfsLzoTextScanner::Open(ScannerContext* context) {
  RETURN_IF_ERROR(HdfsTextScanner::Open(context));
  stream_->set_read_past_size_cb(&HdfsLzoTextScanner::MaxBlockCompressedSize);
  header_ = reinterpret_cast<LzoFileHeader*>(
      static_cast<HdfsScanNodeBase*>(scan_node_)->GetFileMetadata(
          context->partition_descriptor()->id(), stream_->filename()));
  if (header_ == nullptr) {
    only_parsing_header_ = true;
    return Status::OK();
  }

  DCHECK_EQ(only_parsing_header_, false);
  Status status;
  if (stream_->scan_range()->offset() == 0) {
    RETURN_IF_FALSE(stream_->SkipBytes(header_->header_size_, &status));
  } else {
    DCHECK(!header_->offsets.empty());
    bool found_block;
    status = FindFirstBlock(&found_block);
    if (!found_block) eos_ = true;
  }
  if (!status.ok()) RETURN_IF_ERROR(state_->LogOrReturnError(status.msg()));
  return Status::OK();
}

Status HdfsLzoTextScanner::GetNextInternal(RowBatch* row_batch) {
  if (eos_) return Status::OK();

  if (only_parsing_header_) {
    DCHECK(header_ == nullptr);
    // This is the initial scan range just to parse the header
    header_ = state_->obj_pool()->Add(new LzoFileHeader());
    // Parse the header and read the index file.
    Status status = ReadHeader();
    if (!status.ok()) {
      stringstream ss;
      // TODO: remove this here. We should be able to just return the error and
      // hdfs-scan-node should include all the diagnostics related to the stream.
      // e.g. filename, file format, byte position, eosr, etc.
      ss << "Invalid lzo header information: " << stream_->filename();
      status.AddDetail(ss.str());
      return status;
    }
    RETURN_IF_ERROR(ReadIndexFile());
    // Header is parsed, set the metadata in the scan node.
    static_cast<HdfsScanNodeBase*>(scan_node_)->SetFileMetadata(
        context_->partition_descriptor()->id(), stream_->filename(), header_);
    RETURN_IF_ERROR(IssueFileRanges(stream_->filename()));
    eos_ = true;
  } else {
    DCHECK(header_ != nullptr);
    RETURN_IF_ERROR(HdfsTextScanner::GetNextInternal(row_batch));
  }
  return Status::OK();
}

Status HdfsLzoTextScanner::LzoIssueInitialRangesImpl(HdfsScanNodeBase* scan_node,
    const vector<HdfsFileDesc*>& files) {
  vector<ScanRange*> header_ranges;
  // Issue just the header range for each file.  When the header is complete,
  // we'll issue the ranges for that file.  Read the minimum header size plus
  // up to 255 bytes of optional file name.
  for (int i = 0; i < files.size(); ++i) {
    // These files should be filtered by the planner.
    DCHECK(!ends_with(files[i]->filename, HdfsTextScanner::LZO_INDEX_SUFFIX));

    ScanRangeMetadata* metadata =
        reinterpret_cast<ScanRangeMetadata*>(files[i]->splits[0]->meta_data());
    int64_t header_size = min(static_cast<int64_t>(HEADER_SIZE), files[i]->file_length);
    ScanRange* header_range = scan_node->AllocateScanRange(
        files[i]->fs, files[i]->filename.c_str(), header_size, 0, metadata->partition_id,
        -1, false, false, files[i]->mtime);
    header_ranges.push_back(header_range);
  }
  // The files' ranges will be submitted once the header range completes, in
  // IssueFileRanges().  So pass 0 to indicate that no file has been added completely.
  RETURN_IF_ERROR(scan_node->AddDiskIoRanges(header_ranges, 0));
  return Status::OK();
}

Status HdfsLzoTextScanner::IssueFileRanges(const char* filename) {
  DCHECK(header_ != nullptr);
  HdfsFileDesc* file_desc = scan_node_->GetFileDesc(
      context_->partition_descriptor()->id(), filename);
  if (header_->offsets.empty()) {
    // If offsets is empty then there was no index file.  The file cannot be split.
    // If this contains the range starting at offset 0 generate a scan for whole file.
    const vector<ScanRange*>& splits = file_desc->splits;
    ScanRange* zero_offset_range = nullptr;
    for (int j = 0; j < splits.size(); ++j) {
      if (splits[j]->offset() != 0) {
        // There is no index so this file is not splittable. Mark the other initial
        // splits complete.
        scan_node_->RangeComplete(THdfsFileFormat::TEXT, THdfsCompression::LZO);
        continue;
      }
      // There can only be one 0-offset range
      DCHECK(zero_offset_range == nullptr);
      ScanRangeMetadata* metadata =
          reinterpret_cast<ScanRangeMetadata*>(file_desc->splits[0]->meta_data());
      zero_offset_range = scan_node_->AllocateScanRange(
          file_desc->fs, filename, file_desc->file_length, 0, metadata->partition_id,
          -1, false, false, file_desc->mtime);
    }
    // Add the 0-offset range and indicate that the file has no remaining ranges by
    // passing num_files_queued = 1.
    if (zero_offset_range != nullptr) {
      RETURN_IF_ERROR(scan_node_->AddDiskIoRanges(
        vector<ScanRange*>(1, zero_offset_range), 1));
    }
  } else {
    RETURN_IF_ERROR(scan_node_->AddDiskIoRanges(file_desc));
  }
  return Status::OK();
}

Status HdfsLzoTextScanner::ReadIndexFile() {
  string index_filename(stream_->filename());
  index_filename.append(HdfsTextScanner::LZO_INDEX_SUFFIX);

  hdfsFS connection = stream_->scan_range()->fs();
  // If there is no index file we can read the file by starting at the beginning
  // and reading through to the end.
  if (hdfsExists(connection, index_filename.c_str()) != 0) {
    LOG(WARNING) << "No index file for: " << stream_->filename()
                 << ". Split scans are not possible.";
    return Status::OK();
  }

  hdfsFile index_file = hdfsOpenFile(connection,
       index_filename.c_str(), O_RDONLY, 0, 0, 0);

  if (index_file == nullptr) {
    return Status(GetHdfsErrorMsg("Error while opening index file: ", index_filename));
  }

  // TODO: This should go through the I/O manager.
  constexpr uint16_t TARGET_READ_SIZE = 10 * 1024;
  uint8_t buffer[TARGET_READ_SIZE];
  tSize bytes_read;
  uint8_t unprocessed_bytes = 0;

  // We expect that the file size be a multiple of sizeof(uint64_t). However, we may not
  // always get a buffer of a size that is a multiple of sizeof(uint64_t) from hdfsRead().
  // We carry over the (buffer size % sizeof(uint64_t)) from every hdfsRead() every time
  // and process it in the next iteration of the loop so as to not over look some bytes.
  while ((bytes_read = hdfsRead(connection, index_file,
      buffer + unprocessed_bytes, TARGET_READ_SIZE - unprocessed_bytes)) > 0) {
    bytes_read += unprocessed_bytes;
    unprocessed_bytes = bytes_read % sizeof(uint64_t);

    // Round down to the nearset multiple of size(uint64_t).
    uint16_t read_until = bytes_read - unprocessed_bytes;
    // Interpret bytes as a series of 64-bit offsets.
    for (uint8_t* bp = buffer; bp < buffer + read_until; bp += sizeof(uint64_t)) {
      int64_t offset = ReadWriteUtil::GetInt<uint64_t>(bp);
      header_->offsets.push_back(offset);
    }
    // Move over the remaining 0-7 bytes that haven't been processed to the beginning of
    // the buffer.
    for (uint8_t i = 0; i < unprocessed_bytes; ++i) {
      buffer[i] = buffer[read_until + i];
    }
  }

  // If there are any left over bytes, they are deliberately ignored.
  int close_stat = hdfsCloseFile(connection, index_file);

  if (bytes_read == -1) {
    return Status(GetHdfsErrorMsg("Error while reading index file: ", index_filename));
  }

  if (close_stat == -1) {
    return Status(GetHdfsErrorMsg("Error while closing index file: ", index_filename));
  }

  return Status::OK();
}

Status HdfsLzoTextScanner::FindFirstBlock(bool* found) {
  int64_t offset = stream_->file_offset();

  // Find the first block at or after the current file offset.  That way the
  // scan will start, or restart, on a block boundary.
  vector<int64_t>::iterator pos =
      upper_bound(header_->offsets.begin(), header_->offsets.end(), offset);

  if (pos == header_->offsets.end()) {
    // In this case, the scan range started past the end of the last block. Skip
    // this as the previous scan range is responsible for it.
    *found = false;
    return Status::OK();
  }

  if (*pos > offset + stream_->scan_range()->len()) {
    // In this case, the scan range does not contain the start of any blocks.
    // This scan range is then not responsible for any bytes.
    *found = false;
    return Status::OK();
  }

  VLOG_ROW << "First Block: " << stream_->filename()
           << " for " << offset << " @" << *pos;
  Status status;
  if (!stream_->SkipBytes(*pos - offset, &status)) return status;
  *found = true;
  return status;
}

Status HdfsLzoTextScanner::ReadData(MemPool* pool) {
  do {
    Status status = ReadAndDecompressData(pool);
    if (status.ok()) return Status::OK();
    RETURN_IF_ERROR(state_->LogOrReturnError(status.msg()));

    // On error try to skip forward to the next block.
    bool found_block;
    status = FindFirstBlock(&found_block);
    if (!status.ok() || !found_block) {
      if (!status.ok()) RETURN_IF_ERROR(state_->LogOrReturnError(status.msg()));

      // Just force to end of file, we cannot do more recovery if we can't find
      // the next block
      eos_read_ = true;
      bytes_remaining_ = 0;
      return Status::OK();
    }
  } while (!stream_->eosr());

  // Reset the scanner state.
  RETURN_IF_ERROR(HdfsTextScanner::ResetScanner());
  return Status::OK();
}

Status HdfsLzoTextScanner::FillByteBuffer(MemPool* pool, bool* eosr, int num_bytes) {
  *eosr = false;
  byte_buffer_read_size_ = 0;

  if (stream_->eof()) {
    *eosr = true;
    return Status::OK();
  }

  // Figure out if we have enough data and read more if necessary.
  if ((num_bytes == 0 && bytes_remaining_ == 0) || num_bytes > bytes_remaining_) {
    // Read and decompress the next block.
    RETURN_IF_ERROR(ReadData(pool));
  }

  if (bytes_remaining_ != 0) {
    if (bytes_remaining_ >= num_bytes) {
      // We have enough bytes left to fill the request.
      byte_buffer_ptr_ = reinterpret_cast<char*>(block_buffer_ptr_);
      if (num_bytes == 0) {
         byte_buffer_read_size_ = bytes_remaining_;
      } else {
         byte_buffer_read_size_ = num_bytes;
      }
    } else {
      byte_buffer_ptr_ = reinterpret_cast<char*>(block_buffer_ptr_);
      byte_buffer_read_size_ = bytes_remaining_;
    }
    // We assume a block is larger than the largest request.
    if (!eos_read_ && num_bytes > bytes_remaining_) {
      // Text only reads everything or 1024 so we do not need to handle this case.
      DCHECK(false) << "Unexpected read size: " << num_bytes << " " << bytes_remaining_;
      return Status("Unexpected read size in LZO decompressor");
    }
  }

  byte_buffer_end_ = byte_buffer_ptr_ + byte_buffer_read_size_;
  if (bytes_remaining_ != 0) {
    bytes_remaining_ -= byte_buffer_read_size_;
    block_buffer_ptr_ += byte_buffer_read_size_;
  }

  *eosr = stream_->eosr() || (eos_read_ && bytes_remaining_ == 0);

  if (VLOG_ROW_IS_ON && *eosr) {
    VLOG_ROW << "Returning eosr for: " << stream_->filename()
             << " @" << stream_->file_offset();
  }
  return Status::OK();
}

Status HdfsLzoTextScanner::Checksum(LzoChecksum type, const string& source,
    int expected_checksum, uint8_t* buffer, int length) {

  if (disable_checksum_) return Status::OK();

  // Do the checksum if requested.
  int32_t calculated_checksum = 0;
  switch (type) {
    case CHECK_NONE:
      return Status::OK();

    case CHECK_CRC32:
      calculated_checksum = lzo_crc32(CRC32_INIT_VALUE, buffer, length);
      break;

    case CHECK_ADLER:
      calculated_checksum = lzo_adler32(ADLER32_INIT_VALUE, buffer, length);
      break;

    default:
      DCHECK(false) << "Should have been handled when parsing metadata.";
  }

  if (calculated_checksum != expected_checksum) {
    stringstream ss;
    ss << "Checksum of " << source << " block failed on file: " << stream_->filename()
       << " at offset: " << stream_->file_offset() - length
       << " expected: " << expected_checksum << " got: " << calculated_checksum;
    return Status(ss.str());
  }
  return Status::OK();
}

Status HdfsLzoTextScanner::ReadHeader() {
  uint8_t* magic;
  int64_t bytes_read;
  Status status;
  // Read the header in. HEADER_SIZE over estimates the maximum header.
  RETURN_IF_FALSE(stream_->GetBytes(HEADER_SIZE, &magic, &bytes_read, &status));

  if (bytes_read < MIN_HEADER_SIZE) {
    stringstream ss;
    ss << "File is too short. File size: " << bytes_read;
    return Status(ss.str());
  }

  if (memcmp(magic, LZOP_MAGIC, sizeof(LZOP_MAGIC))) {
    stringstream ss;
    ss << "Invalid LZOP_MAGIC: '"
       << ReadWriteUtil::HexDump(magic, sizeof(LZOP_MAGIC)) << "'" << endl;
    return Status(ss.str());
  }

  uint8_t* header = magic + sizeof(LZOP_MAGIC);
  uint8_t* h_ptr = header;

  int version = ReadWriteUtil::GetInt<uint16_t>(h_ptr);
  if (version > LZOP_VERSION) {
    stringstream ss;
    ss << "Compressed with later version of lzop: " << version
       << " must be less than: " << LZOP_VERSION;
    return Status(ss.str());
  }
  h_ptr += sizeof(int16_t);

  int libversion = ReadWriteUtil::GetInt<uint16_t>(h_ptr);
  if (libversion < MIN_LZO_VERSION) {
    stringstream ss;
    ss << "Compressed with incompatible lzo version: " << version
       << "must be at least: " << MIN_ZOP_VERSION;
    return Status(ss.str());
  }
  h_ptr += sizeof(int16_t);

  // The version of LZOP needed to interpret this file.
  int neededversion = ReadWriteUtil::GetInt<uint16_t>(h_ptr);
  if (neededversion > LZOP_VERSION) {
    stringstream ss;
    ss << "Compressed with imp incompatible lzo version: " << neededversion
       << "must be at no more than: " << LZOP_VERSION;
    return Status(ss.str());
  }
  h_ptr += sizeof(int16_t);

  uint8_t method = *h_ptr++;
  if (method < 1 || method > 3) {
    stringstream ss;
    ss << "Invalid compression method: " << method;
    return Status(ss.str());
  }
  uint8_t level = *h_ptr++;

  int flags = ReadWriteUtil::GetInt<uint32_t>(h_ptr);
  LzoChecksum header_checksum = (flags & F_H_CRC32) ? CHECK_CRC32 : CHECK_ADLER;
  header_->output_checksum_type_ = (flags & F_CRC32_D) ? CHECK_CRC32 :
      (flags & F_ADLER32_D) ? CHECK_ADLER : CHECK_NONE;
  header_->input_checksum_type_ = (flags & F_CRC32_C) ? CHECK_CRC32 :
      (flags & F_ADLER32_C) ? CHECK_ADLER : CHECK_NONE;

  if (flags & (F_RESERVED | F_MULTIPART | F_H_FILTER)) {
    stringstream ss;
    ss << "Unsupported flags: " << flags;
    return Status(ss.str());
  }
  h_ptr += sizeof(int32_t);

  // skip mode and time fields
  h_ptr += 3 * sizeof(int32_t);

  // Skip filename.
  h_ptr += *h_ptr + 1;

  // The header always has a checksum.
  int32_t expected_checksum = ReadWriteUtil::GetInt<uint32_t>(h_ptr);
  int32_t computed_checksum;
  if (header_checksum == CHECK_CRC32) {
    computed_checksum = CRC32_INIT_VALUE;
    computed_checksum = lzo_crc32(computed_checksum, header, h_ptr - header);
  } else {
    computed_checksum = ADLER32_INIT_VALUE;
    computed_checksum = lzo_adler32(computed_checksum, header, h_ptr - header);
  }

  if (computed_checksum != expected_checksum) {
    stringstream ss;
    ss << "Invalid header checksum: " << computed_checksum
       << " expected: " << expected_checksum;
    return Status(ss.str());
  }
  h_ptr += sizeof(int32_t);

  // Skip the extra field if any.
  if (flags & F_H_EXTRA_FIELD) {
    int32_t len;
    Status extra_status;
    if (!stream_->ReadInt(&len, &extra_status)) return extra_status;
    // Add the size of the len and the checksum and the len to the total h_ptr size.
    h_ptr += (2 * sizeof(int32_t)) + len;
  }

  VLOG_FILE << "Reading: " << stream_->filename() << " Header: version: " << version
            << "(" << libversion << "/" << neededversion << ")"
            << " method: " << (int)method << "@" << (int)level
            << " flags: " << flags;

  RETURN_IF_ERROR(status);

  header_->header_size_ = h_ptr - magic;
  return Status::OK();
}

Status HdfsLzoTextScanner::ReadAndDecompressData(MemPool* pool) {
  bytes_remaining_ = 0;
  Status status;

  // Read the uncompressed
  int32_t uncompressed_len = 0, compressed_len = 0;
  RETURN_IF_FALSE(stream_->ReadInt(&uncompressed_len, &status));
  if (uncompressed_len == 0) {
    DCHECK(stream_->eosr());
    eos_read_ = true;
    return Status::OK();
  }
  if (uncompressed_len < 0) {
    stringstream ss;
    ss << "Corrupt lzo file. Invalid uncompressed length: " << uncompressed_len
       << " in file: " << stream_->filename();
    return Status(ss.str());
  }

  // Read the compressed len
  RETURN_IF_FALSE(stream_->ReadInt(&compressed_len, &status));

  if (compressed_len > LZO_MAX_BLOCK_SIZE) {
    stringstream ss;
    ss << "Blocksize: " << compressed_len << " is greater than LZO_MAX_BLOCK_SIZE: "
       << LZO_MAX_BLOCK_SIZE;
    return Status(ss.str());
  }

  int out_checksum;
  // The checksum of the uncompressed data.
  if (header_->output_checksum_type_ != CHECK_NONE) {
    RETURN_IF_FALSE(stream_->ReadInt(&out_checksum, &status));
  }

  int in_checksum = 0;
  if (compressed_len < uncompressed_len && header_->input_checksum_type_ != CHECK_NONE) {
    RETURN_IF_FALSE(stream_->ReadInt(&in_checksum, &status));
  } else {
    // If the compressed data size is equal to the uncompressed data size, then
    // the uncompressed data is stored and there is no compressed checksum.
    in_checksum = out_checksum;
  }

  // Read in the compressed data
  uint8_t* compressed_data;
  int64_t bytes_read;
  RETURN_IF_FALSE(
      stream_->GetBytes(compressed_len, &compressed_data, &bytes_read, &status));
  if (bytes_read == 0) {
    DCHECK(stream_->eof());
    DCHECK_EQ(bytes_remaining_, 0);
    if (compressed_len != 0 && state_->abort_on_error()) {
      // The last block might be empty if it is the end of the file.
      stringstream ss;
      ss << "Last lzo block missing. Expected block size: " << compressed_len;
      return Status(ss.str());
    }
    return Status::OK();
  } else if (compressed_len != bytes_read) {
    stringstream ss;
    ss << "Corrupt lzo file. Compressed block should have length '"
       << compressed_len << "' but could only read '" << bytes_read << "' from file: "
       << stream_->filename();
    return Status(ss.str());
  }
  context_->ReleaseCompletedResources(false);
  eos_read_ = stream_->eosr();

  // Checksum the data.
  RETURN_IF_ERROR(Checksum(header_->input_checksum_type_,
      "compressed", in_checksum, compressed_data, compressed_len));

  // Attach any data that previously returned string slots may reference.
  bool has_string_slots = !scan_node_->tuple_desc()->string_slots().empty();
  if (has_string_slots) {
    if (pool != nullptr) {
      pool->AcquireData(block_buffer_pool_.get(), false);
    } else {
      block_buffer_pool_->FreeAll();
    }
    block_buffer_len_ = 0;
    block_buffer_ = block_buffer_ptr_ = nullptr;
  }

  // If the compressed length is the same as the uncompressed length, it means the data
  // was not compressed. If there are string slots, we need to copy the data out so it can
  // be returned.
  if (compressed_len == uncompressed_len) {
    if (has_string_slots) {
      DCHECK_EQ(0, block_buffer_len_);
      block_buffer_ptr_ = block_buffer_ = block_buffer_pool_->Allocate(uncompressed_len);
      block_buffer_len_ = uncompressed_len;
      memcpy(block_buffer_ptr_, compressed_data, uncompressed_len);
    } else {
      block_buffer_ptr_ = compressed_data;
    }
    bytes_remaining_ = uncompressed_len;
    return Status::OK();
  }


  if (uncompressed_len > block_buffer_len_) {
    block_buffer_ = block_buffer_pool_->Allocate(uncompressed_len);
    block_buffer_len_ = uncompressed_len;
  }
  block_buffer_ptr_ = block_buffer_;
  bytes_remaining_ = uncompressed_len;

  // IMPALA-5172: lzo_uint is a 64-bit datatype. &uncompressed_len cannot be cast to
  // an lzo_uint*, as it points to a 32-bit integer. Use a temporary 64-bit variable to
  // interact with lzo1x_decompress_safe(). Since the variable won't be set to a value
  // greater than what was passed in, it can safely be assigned to a 32 bit integer
  // afterward.
  uint64_t uncompressed_len_64bit = static_cast<uint64_t>(uncompressed_len);

  // Decompress the data.  lzop always uses lzo1x.
  SCOPED_TIMER(decompress_timer_);
  int ret = lzo1x_decompress_safe(compressed_data, compressed_len,
      block_buffer_, &uncompressed_len_64bit, nullptr);
  DCHECK_LE(uncompressed_len_64bit, uncompressed_len);
  uncompressed_len = static_cast<int32_t>(uncompressed_len_64bit);

  if (ret != LZO_E_OK || bytes_remaining_ != uncompressed_len) {
    // Avoid accumulating memory with repeated decompression failures.
    block_buffer_pool_->Clear();
    stringstream ss;
    ss << "Lzo decompression failed on file: " << stream_->filename()
       << " at offset: " << stream_->file_offset() << " returned: " << ret
       << " output size: " << uncompressed_len << " expected: " << block_buffer_len_;
    return Status(ss.str());
  }

  // Do the checksum if requested.
  Status checksum_status = Checksum(header_->output_checksum_type_,
     "decompressed", out_checksum, block_buffer_, uncompressed_len);
  if (!checksum_status.ok()) {
    // Avoid accumulating memory with repeated checksum mismatches.
    block_buffer_pool_->Clear();
    return checksum_status;
  }

  // Return end of scan range even if there are bytes in the disk buffer.
  // We fetched the next disk buffer past EOSR to complete the read of this compressed
  // block.  When the scanner finishes with the data we return here it must
  // go into Finish mode and complete its final row.
  eos_read_ = stream_->eosr();
  VLOG_ROW << "LZO decompressed " << uncompressed_len << " bytes from "
           << stream_->filename() << " @" << stream_->file_offset() - compressed_len;
  return Status::OK();
}

}
