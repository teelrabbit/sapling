/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

#include "eden/fs/model/TreeEntry.h"

#include <sys/stat.h>
#include <ostream>

#include <folly/Conv.h>
#include <folly/Range.h>
#include <folly/logging/xlog.h>

#include "eden/fs/utils/EnumValue.h"
#include "eden/fs/utils/PathFuncs.h"

namespace facebook::eden {

using namespace folly;
using namespace folly::io;

mode_t modeFromTreeEntryType(TreeEntryType ft) {
  switch (ft) {
    case TreeEntryType::TREE:
      return S_IFDIR | 0755;
    case TreeEntryType::REGULAR_FILE:
      return S_IFREG | 0644;
    case TreeEntryType::EXECUTABLE_FILE:
      return S_IFREG | 0755;
    case TreeEntryType::SYMLINK:
#ifdef _WIN32
      // On Windows, we report symlinks as files. The behaviour here is same as
      // Mercurial.
      // TODO: would be nice to log some useful context here!
      return S_IFREG | 0755;
#else
      return S_IFLNK | 0755;
#endif
  }
  XLOG(FATAL) << "illegal file type " << enumValue(ft);
}

std::optional<TreeEntryType> treeEntryTypeFromMode(mode_t mode) {
  if (S_ISREG(mode)) {
#ifdef _WIN32
    // On Windows, S_ISREG only means regular file and doesn't support
    // TreeEntryType::EXECUTABLE_FILE and TreeEntryType::SYMLINK
    return TreeEntryType::REGULAR_FILE;
#else
    return mode & S_IXUSR ? TreeEntryType::EXECUTABLE_FILE
                          : TreeEntryType::REGULAR_FILE;
  } else if (S_ISLNK(mode)) {
    return TreeEntryType::SYMLINK;
#endif
  } else if (S_ISDIR(mode)) {
    return TreeEntryType::TREE;
  } else {
    return std::nullopt;
  }
}

std::string TreeEntry::toLogString() const {
  char fileTypeChar = '?';
  switch (type_) {
    case TreeEntryType::TREE:
      fileTypeChar = 'd';
      break;
    case TreeEntryType::REGULAR_FILE:
      fileTypeChar = 'f';
      break;
    case TreeEntryType::EXECUTABLE_FILE:
      fileTypeChar = 'x';
      break;
    case TreeEntryType::SYMLINK:
      fileTypeChar = 'l';
      break;
  }

  return folly::to<std::string>(
      "(", name_, ", ", hash_, ", ", fileTypeChar, ")");
}

std::ostream& operator<<(std::ostream& os, TreeEntryType type) {
  switch (type) {
    case TreeEntryType::TREE:
      return os << "TREE";
    case TreeEntryType::REGULAR_FILE:
      return os << "REGULAR_FILE";
    case TreeEntryType::EXECUTABLE_FILE:
      return os << "EXECUTABLE_FILE";
    case TreeEntryType::SYMLINK:
      return os << "SYMLINK";
  }

  return os << "TreeEntryType::" << int(type);
}

bool operator==(const TreeEntry& entry1, const TreeEntry& entry2) {
  return (entry1.getHash() == entry2.getHash()) &&
      (entry1.getType() == entry2.getType()) &&
      (entry1.getName() == entry2.getName());
}

bool operator!=(const TreeEntry& entry1, const TreeEntry& entry2) {
  return !(entry1 == entry2);
}

size_t TreeEntry::getIndirectSizeBytes() const {
  // TODO: we should consider using a standard memory framework across
  // eden for this type of thing. D17174143 is one such idea.
  return estimateIndirectMemoryUsage(name_.value());
}

size_t TreeEntry::serializedSize() const {
  return sizeof(uint8_t) + sizeof(uint16_t) + hash_.size() + sizeof(uint16_t) +
      name_.stringPiece().size() + sizeof(uint64_t) + Hash20::RAW_SIZE;
}

void TreeEntry::serialize(Appender& appender) const {
  appender.write<uint8_t>(static_cast<uint8_t>(type_));
  auto hash = hash_.getBytes();
  XCHECK_LE(hash.size(), std::numeric_limits<uint16_t>::max());
  appender.write<uint16_t>(folly::to_narrow(hash.size()));
  appender.push(hash);
  auto name = name_.stringPiece();
  XCHECK_LE(name.size(), std::numeric_limits<uint16_t>::max());
  appender.write<uint16_t>(folly::to_narrow(name.size()));
  appender.push(name);
  if (size_) {
    appender.write<uint64_t>(*size_);
  } else {
    appender.write<uint64_t>(NO_SIZE);
  }
  if (contentSha1_) {
    appender.push(contentSha1_->getBytes());
  } else {
    appender.push(kZeroHash.getBytes());
  }
}

std::optional<TreeEntry> TreeEntry::deserialize(folly::StringPiece& data) {
  uint8_t type;
  if (data.size() < sizeof(uint8_t)) {
    XLOG(ERR) << "Can not read tree entry type, bytes remaining "
              << data.size();
    return std::nullopt;
  }
  memcpy(&type, data.data(), sizeof(uint8_t));
  data.advance(sizeof(uint8_t));

  uint16_t hash_size;
  if (data.size() < sizeof(uint16_t)) {
    XLOG(ERR) << "Can not read tree entry hash size, bytes remaining "
              << data.size();
    return std::nullopt;
  }
  memcpy(&hash_size, data.data(), sizeof(uint16_t));
  data.advance(sizeof(uint16_t));

  if (data.size() < hash_size) {
    XLOG(ERR) << "Can not read tree entry hash, bytes remaining " << data.size()
              << " need " << hash_size;
    return std::nullopt;
  }
  auto hash_bytes = ByteRange{StringPiece{data, 0, hash_size}};
  auto hash = ObjectId{hash_bytes};
  data.advance(hash_size);

  uint16_t name_size;
  if (data.size() < sizeof(uint16_t)) {
    XLOG(ERR) << "Can not read tree entry name size, bytes remaining "
              << data.size();
    return std::nullopt;
  }
  memcpy(&name_size, data.data(), sizeof(uint16_t));
  data.advance(sizeof(uint16_t));

  if (data.size() < name_size) {
    XLOG(ERR) << "Can not read tree entry name, bytes remaining " << data.size()
              << " need " << name_size;
    return std::nullopt;
  }
  auto name_bytes = StringPiece{data, 0, name_size};
  auto name = PathComponent{name_bytes};
  data.advance(name_size);

  if (data.size() < sizeof(uint64_t)) {
    XLOG(ERR) << "Can not read tree entry size, bytes remaining "
              << data.size();
    return std::nullopt;
  }
  uint64_t size_bytes;
  memcpy(&size_bytes, data.data(), sizeof(uint64_t));
  data.advance(sizeof(uint64_t));
  std::optional<uint64_t> size;
  if (size_bytes == NO_SIZE) {
    size = std::nullopt;
  } else {
    size = size_bytes;
  }

  if (data.size() < Hash20::RAW_SIZE) {
    XLOG(ERR) << "Can not read tree entry sha1, bytes remaining "
              << data.size();
    return std::nullopt;
  }
  Hash20::Storage sha1_bytes;
  memcpy(&sha1_bytes, data.data(), Hash20::RAW_SIZE);
  data.advance(Hash20::RAW_SIZE);
  Hash20 sha1_raw = Hash20{sha1_bytes};
  std::optional<Hash20> sha1;
  if (sha1_raw == kZeroHash) {
    sha1 = std::nullopt;
  } else {
    sha1 = sha1_raw;
  }

  return TreeEntry{hash, name, (TreeEntryType)type, size, sha1};
}

} // namespace facebook::eden
