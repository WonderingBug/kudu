// Copyright (c) 2012, Cloudera, inc.
#ifndef KUDU_TABLET_CONCURRENT_BTREE_H
#define KUDU_TABLET_CONCURRENT_BTREE_H

#include <boost/noncopyable.hpp>
#include <boost/static_assert.hpp>
#include <boost/smart_ptr/detail/yield_k.hpp>
#include <boost/utility/binary.hpp>
#include "util/stringbag.h"
#include "gutil/spinlock_wait.h"
#include "gutil/stringprintf.h"
#include "gutil/port.h"

namespace kudu { namespace tablet {
namespace btree {

struct BTreeTraits;

template<class Traits>
class InternalNode;
template<class Traits>
class LeafNode;
template<class Traits>
class CBTree;
template<class Traits>
class NodeBase;

typedef base::subtle::Atomic64 AtomicVersion;

struct VersionField {
public:
  static AtomicVersion StableVersion(volatile AtomicVersion *version) {
    for (int loop_count = 0; true; loop_count++) {
      AtomicVersion v_acq = base::subtle::Acquire_Load(version);
      if (!IsLocked(v_acq)) {
        return v_acq;
      }
      boost::detail::yield(loop_count++);
    }
  }

  static void Lock(volatile AtomicVersion *version) {
    int loop_count = 0;

    while (true) {
      AtomicVersion v_acq = base::subtle::Acquire_Load(version);
      if (!IsLocked(v_acq)) {
        AtomicVersion v_locked = SetLockBit(v_acq, 1);
        if (base::subtle::Acquire_CompareAndSwap(version, v_acq, v_locked) == v_acq) {
          return;
        }
      }
      // Either was already locked by someone else, or CAS failed.
      boost::detail::yield(loop_count++);
    }
  }

  static void Unlock(volatile AtomicVersion *version) {
    // NoBarrier should be OK here, because no one else modifies the
    // version while we have it locked.
    AtomicVersion v = base::subtle::NoBarrier_Load(version);

    DCHECK(v & BTREE_LOCK_MASK);

    // If splitting, increment the splitting field
    v += ((v & BTREE_SPLITTING_MASK) >> BTREE_SPLITTING_BIT) << BTREE_VSPLIT_SHIFT;
    // If inserting, increment the insert field
    v += ((v & BTREE_INSERTING_MASK) >> BTREE_INSERTING_BIT) << BTREE_VINSERT_SHIFT;

    // Get rid of the lock, flags and any overflow into the unused section.
    v = SetLockBit(v, 0);
    v &= ~(BTREE_UNUSED_MASK | BTREE_INSERTING_MASK | BTREE_SPLITTING_MASK);

    base::subtle::Release_Store(version, v);
  }

  static uint64_t GetVSplit(AtomicVersion v) {
    return v & BTREE_VSPLIT_MASK;
  }
  static uint64_t GetVInsert(AtomicVersion v) {
    return (v & BTREE_VINSERT_MASK) >> BTREE_VINSERT_SHIFT;
  }
  static void SetSplitting(volatile AtomicVersion *v) {
    base::subtle::Release_Store(v, *v | BTREE_SPLITTING_MASK);
  }
  static void SetInserting(volatile AtomicVersion *v) {
    base::subtle::Release_Store(v, *v | BTREE_INSERTING_MASK);
  }

  // Return true if the two version fields differ in more
  // than just the lock status.
  static bool IsDifferent(AtomicVersion v1, AtomicVersion v2) {
    return PREDICT_FALSE((v1 & ~BTREE_LOCK_MASK) != (v2 & ~BTREE_LOCK_MASK));
  }

  // Return true if a split has occurred between the two versions
  // or is currently in progress
  static bool HasSplit(AtomicVersion v1, AtomicVersion v2) {
    return PREDICT_FALSE((v1 & (BTREE_VSPLIT_MASK | BTREE_SPLITTING_MASK)) !=
                         (v2 & (BTREE_VSPLIT_MASK | BTREE_SPLITTING_MASK)));
  }

  static inline bool IsLocked(AtomicVersion v) {
    return v & BTREE_LOCK_MASK;
  }

  static string Stringify(AtomicVersion v) {
    return StringPrintf("[flags=%c%c%c vins=%ld vsplit=%ld]",
                        (v & BTREE_LOCK_MASK) ? 'L':' ',
                        (v & BTREE_SPLITTING_MASK) ? 'S':' ',
                        (v & BTREE_INSERTING_MASK) ? 'I':' ',
                        GetVInsert(v),
                        GetVSplit(v));
  }

private:
  enum {
    BTREE_LOCK_BIT = 63,
    BTREE_SPLITTING_BIT = 62,
    BTREE_INSERTING_BIT = 61,
    BTREE_VINSERT_SHIFT = 27,
    BTREE_VSPLIT_SHIFT = 0,

#define BB(x) BOOST_BINARY(x)
    BTREE_LOCK_MASK =
    BB(10000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 ),
    BTREE_SPLITTING_MASK =
    BB(01000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 ),
    BTREE_INSERTING_MASK =
    BB(00100000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 ),

    // There is one unused byte between the single-bit fields and the
    // incremented fields. This allows us to efficiently increment the
    // fields and avoid an extra instruction or two, since we don't need
    // to worry about overflow. If vsplit overflows into vinsert, that's
    // not a problem, since the vsplit change always results in a retry.
    // If we roll over into this unused bit, we'll mask it out.
    BTREE_UNUSED_MASK =
    BB(00010000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 ),
    BTREE_VINSERT_MASK =
    BB(00001111 11111111 11111111 11111111 11111100 00000000 00000000 00000000 ),
    BTREE_VSPLIT_MASK =
    BB(00000000 00000000 00000000 00000000 00000011 11111111 11111111 11111111 ),
#undef BB
  };

  //Undeclared constructor - this is just static utilities.
  VersionField();

  static AtomicVersion SetLockBit(AtomicVersion v, int lock) {
    DCHECK(lock == 0 || lock == 1);
    v = v & ~BTREE_LOCK_MASK;
    BOOST_STATIC_ASSERT(sizeof(AtomicVersion) == 8);
    v |= (uint64_t)lock << BTREE_LOCK_BIT;
    return v;
  }
};


enum NodeType {
  INTERNAL_NODE,
  LEAF_NODE
};

// Return the index of the first entry in the bag which is
// >= the given value
template<class T>
size_t FindInSortedBag(const StringBag<T> &bag, ssize_t bag_size,
                       const Slice &key, bool *exact) {
  DCHECK_GE(bag_size, 0);
  for (int i = 0; i < bag_size; i++) {
    int compare = bag.Get(i).compare(key);
    if (compare >= 0) {
      // key in bag >= expected value
      *exact = compare == 0;
      return i;
    }
  }

  *exact = false;
  // search key is greater than all keys in the node --
  // insertion point at the end
  return bag_size;
}

template<class Traits>
class NodeBase {
public:
  AtomicVersion StableVersion() {
    return VersionField::StableVersion(&version_);
  }

  AtomicVersion AcquireVersion() {
    return base::subtle::Acquire_Load(&version_);
  }

  void Lock() {
    VersionField::Lock(&version_);
  }

  bool IsLocked() {
    return VersionField::IsLocked(version_);
  }

  void Unlock() {
    VersionField::Unlock(&version_);
  }

  void SetSplitting() {
    VersionField::SetSplitting(&version_);
  }

  void SetInserting() {
    VersionField::SetInserting(&version_);
  }

  // Return the parent node for this node, with the lock acquired.
  InternalNode<Traits> *GetLockedParent() {
    while (true) {
      InternalNode<Traits> *ret = parent_;
      if (ret == NULL) {
        return NULL;
      }

      ret->Lock();

      if (PREDICT_FALSE(parent_ != ret)) {
        // My parent changed after accomplishing the lock
        ret->Unlock();
        continue;
      }

      return ret;
    }
  }

protected:
  friend class VersionStabilityGuard;
  friend class VersionLockGuard;
  friend class CBTree<Traits>;

  NodeBase() : version_(0), parent_(NULL)
  {}

public:
  volatile AtomicVersion version_;

  // parent_ field is protected not by this node's lock, but by
  // the parent's lock. This allows reassignment of the parent_
  // field to occur after a split without gathering locks for all
  // the children.
  InternalNode<Traits> *parent_;
} PACKED;

// Wrapper around a void pointer, which encodes the type
// of the pointed-to object in its least-significant-bit.
// The pointer may reference either an internal node or a
// leaf node.
// This assumes that the true pointed-to object is at least
// 2-byte aligned, so that the LSB can be used as storage.
template<class T>
struct NodePtr {
  NodePtr() : p_(NULL) {}

  NodePtr(InternalNode<T> *p) {
    uintptr_t p_int = reinterpret_cast<uintptr_t>(p);
    DCHECK(!(p_int & 1)) << "Pointer must be word-aligned";
    p_ = p;
  }

  NodePtr(LeafNode<T> *p) {
    uintptr_t p_int = reinterpret_cast<uintptr_t>(p);
    DCHECK(!(p_int & 1)) << "Pointer must be word-aligned";
    p_ = reinterpret_cast<void *>(p_int | 1);
  }

  NodeType type() {
    DCHECK(p_ != NULL);
    if (reinterpret_cast<uintptr_t>(p_) & 1) {
      return LEAF_NODE;
    } else {
      return INTERNAL_NODE;
    }
  }

  bool is_null() {
    return p_ == NULL;
  }

  InternalNode<T> *internal_node_ptr() {
    DCHECK_EQ(type(), INTERNAL_NODE);
    return reinterpret_cast<InternalNode<T> *>(p_);
  }

  LeafNode<T> *leaf_node_ptr() {
    DCHECK_EQ(type(), LEAF_NODE);
    return reinterpret_cast<LeafNode<T> *>(
      reinterpret_cast<uintptr_t>(p_) & (~1));
  }

  NodeBase<T> *base_ptr() {
    DCHECK(!is_null());
    return reinterpret_cast<NodeBase<T> *>(
      reinterpret_cast<uintptr_t>(p_) & (~1));
  }

  void *p_;
} PACKED;


class VersionStabilityGuard : boost::noncopyable {
public:
  template <class T>
  VersionStabilityGuard(const NodeBase<T> *node) :
    vptr_(&node->version_),
    acquired_val(0)
  {}

  void Acquire() {
    acquired_val = base::subtle::Acquire_Load(vptr_);
  }

  bool WasUnstable() {
    AtomicVersion new_val = base::subtle::Release_Load(vptr_);
    return new_val != acquired_val;
  }

private:
  const volatile AtomicVersion *vptr_;

  AtomicVersion acquired_val;
};

class VersionLockGuard : boost::noncopyable {
public:
  template<class T>
  VersionLockGuard(NodeBase<T> *node) :
    vptr_(&node->version_)
  {
    VersionField::Lock(vptr_);
  }

  ~VersionLockGuard() {
    VersionField::Unlock(vptr_);
  }

private:
  volatile AtomicVersion *vptr_;
};



enum InsertStatus {
  INSERT_SUCCESS,
  INSERT_FULL,
  INSERT_DUPLICATE
};

////////////////////////////////////////////////////////////
// Internal node
////////////////////////////////////////////////////////////

template<class Traits>
class PACKED InternalNode : public NodeBase<Traits> {
public:

  // Construct a new internal node, containing the given children.
  // This also reassigns the parent pointer of the children.
  // Because other accessors of the tree may follow the children's
  // parent pointers back up to discover a new root, and the parent
  // pointers are covered by their parent's lock, this requires that
  // the new internal node node is constructed in LOCKED state.
  InternalNode(const Slice &split_key,
               NodePtr<Traits> lchild,
               NodePtr<Traits> rchild) :
    num_children_(0),
    key_bag_(Traits::fanout - 1, sizeof(storage_))
  {
    DCHECK_EQ(lchild.type(), rchild.type())
      << "Only expect to create a new internal node on account of a "
      << "split: child nodes should have same type";

    // Just assign the version, instead of using the proper ->Lock()
    // since we don't need a CAS here.
    this->version_ = VersionField::BTREE_LOCK_MASK |
      VersionField::BTREE_INSERTING_MASK;

    // TODO: we need to dynamically configure the size of the
    // nodes so that, with really fat keys, we get at least
    // a fanout of 2, or that we can fall back to indirect storage.
    CHECK(key_bag_.Assign(0, split_key));
    DCHECK_GT(split_key.size(), 0);
    child_pointers_[0] = lchild;
    child_pointers_[1] = rchild;
    ReassignParent(lchild);
    ReassignParent(rchild);

    num_children_ = 2;
  }

  // Return the physical size in memory consumed by the internal node
  // NOTE: this is a fully static property, not dependent on
  // anything but the configured Traits.
  size_t node_size() const {
    return Traits::internal_node_size;
  }

  // Insert a new entry to the internal node.
  //
  // This is typically called after one of its child nodes has split.
  InsertStatus Insert(const Slice &key, NodePtr<Traits> right_child) {
    DCHECK(this->IsLocked());
    DCHECK(this->version_ & VersionField::BTREE_INSERTING_MASK);
    CHECK_GT(key.size(), 0);

    bool exact;
    size_t idx = Find(key, &exact);
    CHECK(!exact)
      << "Trying to insert duplicate key " << key.ToString()
      << " into an internal node! Internal node keys should result "
      << " from splits and therefore be unique.";

    if (PREDICT_FALSE(num_children_ == Traits::fanout)) {
      return INSERT_FULL;
    }

    if (PREDICT_FALSE(!key_bag_.Insert(idx, key_count(), key))) {
      return INSERT_FULL;
    }

    // Insert the child pointer in the right spot in the list
    num_children_++;
    for (int i = num_children_ - 1; i > idx + 1; i--) {
      child_pointers_[i] = child_pointers_[i - 1];
    }
    child_pointers_[idx + 1] = right_child;

    ReassignParent(right_child);

    return INSERT_SUCCESS;
  }

  // Return the node index responsible for the given key.
  // For example, if the key is less than the first discriminating
  // node, returns 0. If it is between 0 and 1, returns 1, etc.
  size_t Find(const Slice &key, bool *exact) {
    return FindInSortedBag(key_bag_, key_count(), key, exact);
  }

  // Find the child whose subtree may contain the given key.
  // Note that this result may be an invalid or incorrect pointer if the
  // caller has not locked the node, in which case OCC should be
  // used to verify it after its usage.
  NodePtr<Traits> FindChild(const Slice &key) {
    bool exact;
    size_t idx = FindInSortedBag(key_bag_, key_count(), key, &exact);
    if (exact) {
      idx++;
    }
    return child_pointers_[idx];
  }

  Slice GetKey(size_t idx) const {
    DCHECK_LT(idx, key_count());
    return key_bag_.Get(idx);
  }

  // Truncates the node, removing entries from the right to reduce
  // to the new size. Also compacts the underlying storage so that all
  // free space is contiguous, allowing for new inserts.
  //
  // Requires that the 
  void TruncateAndCompact(size_t new_num_keys) {
    DCHECK(this->IsLocked());
    DCHECK(this->version_ & VersionField::BTREE_SPLITTING_MASK);
    DCHECK_GT(new_num_keys, 0);

    DCHECK_LT(new_num_keys, key_count());
    key_bag_.TruncateAndCompact(Traits::fanout - 1, new_num_keys);
    num_children_ = new_num_keys + 1;

    // TODO: the following loop is not actually necessary for
    // correctness, but it may result in avoiding bugs
    for (int i = 0; i < num_children_; i++) {
      DCHECK(!child_pointers_[i].is_null());
    }
    for (int i = num_children_; i < Traits::fanout; i++) {
      // reset to NULL
      child_pointers_[i] = NodePtr<Traits>();
    }
  }

  // Prefetch up to the first 4 cachelines of this node.
  void PrefetchMemory() const {
    int len = 4 * CACHELINE_SIZE;
    if (Traits::internal_node_size + 1 < len) {
      len = Traits::internal_node_size + 1;
    }

    for (int i = CACHELINE_SIZE; i < len; i += CACHELINE_SIZE) {
      prefetch((const char *) this + i, PREFETCH_HINT_T0);
    }
  }

  string ToString() const {
    return key_bag_.ToString(Traits::fanout - 1);
  }

  private:
  friend class CBTree<Traits>;

  void ReassignParent(NodePtr<Traits> child) {
    child.base_ptr()->parent_ = this;
  }

  int key_count() const {
    // The node uses N keys to separate N+1 child pointers.
    DCHECK_GT(num_children_, 0);
    return num_children_ - 1;
  }

  uint32_t num_children_;
  static constexpr int storage_size = Traits::internal_node_size
    - sizeof(NodeBase<Traits>)
    - sizeof(num_children_)
    - (sizeof(void *) * Traits::fanout); // child_pointers_

  union {
    StringBag<uint16_t> key_bag_;
    char storage_[storage_size];
  } PACKED;

  NodePtr<Traits> child_pointers_[Traits::fanout];
} PACKED;

////////////////////////////////////////////////////////////
// Leaf node
////////////////////////////////////////////////////////////

template<class Traits>
class LeafNode : public NodeBase<Traits> {
public:

  // Construct a new leaf node.
  // If initially_locked is true, then the new node is created
  // with LOCKED and INSERTING set.
  LeafNode(bool initially_locked) :
    next_(NULL),
    num_entries_(0),
    key_bag_(Traits::leaf_max_entries, sizeof(key_storage_)),
    val_bag_(Traits::leaf_max_entries, sizeof(val_storage_))
  {
    if (initially_locked) {
      // Just assign the version, instead of using the proper ->Lock()
      // since we don't need a CAS here.
      this->version_ = VersionField::BTREE_LOCK_MASK |
        VersionField::BTREE_INSERTING_MASK;
    }
  }

  // TODO: rename this to something less confusing
  size_t node_size() const {
    return Traits::leaf_node_size;
  }

  int num_entries() const { return num_entries_; }


  InsertStatus Insert(const Slice &key, const Slice &val) {
    DCHECK(this->IsLocked());
    DCHECK(this->version_ & VersionField::BTREE_INSERTING_MASK);
    CHECK_GT(key.size(), 0);

    bool exact;
    size_t idx = Find(key, &exact);
    if (PREDICT_FALSE(exact)) {
      return INSERT_DUPLICATE;
    }

    if (PREDICT_FALSE(num_entries_ == Traits::leaf_max_entries)) {
      // Full due to metadata
      return INSERT_FULL;
    }

    if (PREDICT_FALSE(key_bag_.space_available() < key.size() ||
                      val_bag_.space_available() < val.size())) {
      // Full due to data space
      return INSERT_FULL;
    }

    DCHECK_LT(idx, Traits::leaf_max_entries);

    // The following inserts should always succeed because we
    // verified space_available above
    CHECK(key_bag_.Insert(idx, num_entries_, key));
    CHECK(val_bag_.Insert(idx, num_entries_, val));
    num_entries_++;

    return INSERT_SUCCESS;
  }

  // Find the index of the first key which is >= the given
  // search key.
  // If the comparison is equal, then sets *exact to true.
  //
  // Note that, if the lock is not held, this may return
  // bogus results, in which case OCC must be used to verify.
  size_t Find(const Slice &key, bool *exact) const {
    return FindInSortedBag(key_bag_, num_entries_, key, exact);
  }

  // Get the slice corresponding to the nth key.
  //
  // If the caller does not hold the lock, then this Slice
  // may point to arbitrary data, and the result should be only
  // trusted when verified by checking for conflicts.
  Slice GetKey(size_t idx) const {
    return key_bag_.Get(idx);
  }

  // Get the slice corresponding to the nth key and value.
  //
  // If the caller does not hold the lock, then this Slice
  // may point to arbitrary data, and the result should be only
  // trusted when verified by checking for conflicts.
  void Get(size_t idx, Slice *k, Slice *v) const {
    *k = key_bag_.Get(idx);
    *v = val_bag_.Get(idx);
  }

  // Truncates the node, removing entries from the right to reduce
  // to the new size. Also compacts the underlying storage so that all
  // free space is contiguous, allowing for new inserts.
  //
  // Caller must hold the node's lock with the INSERTING flag set.
  void TruncateAndCompact(size_t new_num_entries) {
    DCHECK(this->IsLocked());
    DCHECK(this->version_ & VersionField::BTREE_INSERTING_MASK);

    DCHECK_LT(new_num_entries, num_entries_);
    key_bag_.TruncateAndCompact(Traits::leaf_max_entries, new_num_entries);
    val_bag_.TruncateAndCompact(Traits::leaf_max_entries, new_num_entries);
    num_entries_ = new_num_entries;
  }


  string ToString() const {
    string ret;
    for (int i = 0; i < num_entries_; i++) {
      if (i > 0) {
        ret.append(", ");
      }
      Slice k = key_bag_.Get(i);
      Slice v = val_bag_.Get(i);

      StringAppendF(&ret, "[%.*s=%.*s]",
                    (int)k.size(), k.data(),
                    (int)v.size(), v.data());
    }
    return ret;
  }

private:
  friend class CBTree<Traits>;
  friend class InternalNode<Traits>;

  LeafNode<Traits> *next_;

  uint16_t num_entries_;
  static constexpr int storage_size = Traits::leaf_node_size
    - sizeof(NodeBase<Traits>)
    - sizeof(num_entries_)
    - sizeof(next_);

  // TODO: combine keys and values into the same bag
  // so there isn't a stupid 50/50 split between them
  union {
    StringBag<uint32_t> key_bag_;
    char key_storage_[storage_size/2];
  } PACKED;
  union {
    StringBag<uint32_t> val_bag_;
    char val_storage_[storage_size/2];
  } PACKED;


} PACKED;

struct BTreeTraits {
  static const size_t internal_node_size = 256;
  static const size_t fanout = 16;

  static const size_t leaf_node_size = 256;

  // TODO: this should probably be dynamic, since we'd
  // know the size of the value for fixed size tables
  static const size_t leaf_max_entries = 16;

  static const size_t debug_raciness = 0;
};

////////////////////////////////////////////////////////////
// Tree API
////////////////////////////////////////////////////////////

template<class Traits = BTreeTraits>
class CBTree {
public:
  CBTree() :
    root_(new LeafNode<Traits>(false))
  {
    // TODO: use a custom allocator
  }

  ~CBTree() {
    RecursiveDelete(root_);
  }

  bool Insert(const Slice &key, const Slice &val) {
    while (true) {
      AtomicVersion stable_version;
      LeafNode<Traits> *lnode = TraverseToLeaf(key, &stable_version);
      VLOG(3) << "Inserting into " << StringPrintf("%p", lnode);

      lnode->Lock();
      if (VersionField::HasSplit(lnode->AcquireVersion(), stable_version)) {
        // Retry traversal due to a split
        lnode->Unlock();
        continue;
      }

      return InsertInLeaf(lnode, key, val);
    }
  }

  void DebugPrint() {
    AtomicVersion v;
    DebugPrint(StableRoot(&v), NULL, 0);
    CHECK_EQ(root_.base_ptr()->AcquireVersion(), v)
      << "Concurrent modification during DebugPrint not allowed";
  }

  enum GetResult {
    GET_SUCCESS,
    GET_NOT_FOUND,
    GET_TOO_BIG
  };

  // Get a copy of the given key, storing the result in the
  // provided buffer.
  // Returns SUCCESS and sets *buf_len on success
  // Returns NOT_FOUND if no such key is found
  // Returns TOO_BIG if the key is too large to fit in the provided buffer.
  //   In this case, sets *buf_len to the required buffer size.
  //
  // TODO: this call probably won't be necessary in the final implementation
  GetResult GetCopy(const Slice &key, char *buf, size_t *buf_len) {
    size_t in_buf_len = *buf_len;

    retry_from_root:
    {
      AtomicVersion version;
      LeafNode<Traits> *leaf = CHECK_NOTNULL(TraverseToLeaf(key, &version));

      DebugRacyPoint();

      retry_in_leaf:
      {
        GetResult ret;
        bool exact;
        size_t idx = leaf->Find(key, &exact);
        DebugRacyPoint();

        if (!exact) {
          ret = GET_NOT_FOUND;
        } else {
          Slice key_in_node, val_in_node;
          leaf->Get(idx, &key_in_node, &val_in_node);
          *buf_len = val_in_node.size();

          if (PREDICT_FALSE(val_in_node.size() > in_buf_len)) {
            ret = GET_TOO_BIG;
          } else {
            memcpy(buf, val_in_node.data(), val_in_node.size());
            ret = GET_SUCCESS;
          }
        }

        // Got some kind of result, but may be based on racy data.
        // Verify it.
        AtomicVersion new_version = leaf->StableVersion();
        if (VersionField::HasSplit(version, new_version)) {
          goto retry_from_root;
        } else if (VersionField::IsDifferent(version, new_version)) {
          version = new_version;
          goto retry_in_leaf;
        }
        return ret;
      }
    }
  }

  NodePtr<Traits> StableRoot(AtomicVersion *stable_version) {
    while (true) {
      NodePtr<Traits> node = root_;
      NodeBase<Traits> *node_base = node.base_ptr();
      *stable_version = node_base->StableVersion();

      if (PREDICT_TRUE(node_base->parent_ == NULL)) {
        // Found a good root
        return node;
      } else {
        // root has been swapped out
        root_ = node_base->parent_;
      }
    }
  }

  LeafNode<Traits> *TraverseToLeaf(const Slice &key, AtomicVersion *stable_version) {
    retry_from_root:
    AtomicVersion version = 0;
    NodePtr<Traits> node = StableRoot(&version);
    NodeBase<Traits> *node_base = node.base_ptr();

    while (node.type() != LEAF_NODE) {
      retry_in_node:
      int num_children = node.internal_node_ptr()->num_children_;
      NodePtr<Traits> child = node.internal_node_ptr()->FindChild(key);
      NodeBase<Traits> *child_base = child.base_ptr();

      AtomicVersion child_version = -1;

      if (PREDICT_TRUE(!child.is_null())) {
        child_version = child_base->StableVersion();
      }
      AtomicVersion new_node_version = node_base->AcquireVersion();

      if (VersionField::IsDifferent(version, new_node_version)) {
        new_node_version = node_base->StableVersion();

        if (VersionField::HasSplit(version, new_node_version)) {
          goto retry_from_root;
        } else {
          version = new_node_version;
          goto retry_in_node;
        }
      }
      int new_children = node.internal_node_ptr()->num_children_;
      DCHECK(!child.is_null())
        << "should have changed versions when child was NULL: "
        << "old version: " << VersionField::Stringify(version)
        << " new version: " << VersionField::Stringify(new_node_version)
        << " version now: " << VersionField::Stringify(node_base->AcquireVersion())
        << " num_children: " << num_children << " -> " << new_children;

      node = child;
      node_base = child_base;
      version = child_version;
    }
    *stable_version = version;
    return node.leaf_node_ptr();
  }


private:
  // Utility function that, when Traits::debug_raciness is non-zero
  // (i.e only in debug code), will spin for some amount of time
  // related to that setting.
  // This can be used when trying to debug race conditions, but
  // will compile away in production code.
  void DebugRacyPoint() {
    if (Traits::debug_raciness > 0) {
      boost::detail::yield(Traits::debug_raciness);
    }
  }

  // Dump the tree.
  // Requires that there are no concurrent modifications/
  void DebugPrint(NodePtr<Traits> node,
                  InternalNode<Traits> *expected_parent,
                  int indent) {
    using std::cout;
    using std::endl;

    std::string buf;
    switch (node.type()) {
      case LEAF_NODE:
      {
        LeafNode<Traits> *leaf = node.leaf_node_ptr();
        SStringPrintf(&buf, "%*sLEAF %p: ", indent, "", leaf);
        buf.append(leaf->ToString());
        cout << buf << endl;
        CHECK_EQ(leaf->parent_, expected_parent) << "failed for " << leaf;
        break;
      }
      case INTERNAL_NODE:
      {
        InternalNode<Traits> *inode = node.internal_node_ptr();

        SStringPrintf(&buf, "%*sINTERNAL %p: ", indent, "", inode);
        cout << buf << endl;

        for (int i = 0; i < inode->num_children_; i++) {
          DebugPrint(inode->child_pointers_[i], inode, indent + 4);
          if (i < inode->key_count()) {
            SStringPrintf(&buf, "%*sKEY ", indent + 2, "");
            buf.append(inode->GetKey(i).ToString());
            cout << buf << endl;
          }
        }
        CHECK_EQ(inode->parent_, expected_parent) << "failed for " << inode;
        break;
      }
      default:
        CHECK(0) << "bad node type";
    }
  }

  void RecursiveDelete(NodePtr<Traits> node) {
    switch (node.type()) {
      case LEAF_NODE:
        delete node.leaf_node_ptr();
        break;
      case INTERNAL_NODE:
      {
        InternalNode<Traits> *inode = node.internal_node_ptr();
        for (int i = 0; i < inode->num_children_; i++) {
          RecursiveDelete(inode->child_pointers_[i]);
          inode->child_pointers_[i] = NodePtr<Traits>();
        }
        delete inode;
        break;
      }
      default:
        CHECK(0);
    }
  }

  // Inserts the given key/value into the given leaf node.
  // If the leaf node is already full, handles splitting it and
  // propagating splits up the tree.
  //
  // Precondition:
  //   'node' is locked
  // Postcondition:
  //   'node' is unlocked
  bool InsertInLeaf(LeafNode<Traits> *node,
                    const Slice &key,
                    const Slice &val) {
    DCHECK(node->IsLocked());
    node->SetInserting();
    switch (node->Insert(key, val)) {
      case INSERT_SUCCESS:
        node->Unlock();
        return true;
      case INSERT_DUPLICATE:
        node->Unlock();
        return false;
      case INSERT_FULL:
        return SplitLeafAndInsertUp(node, key, val);
        // SplitLeafAndInsertUp takes care of unlocking
      default:
        CHECK(0) << "Unexpected result";
        break;
    }
    CHECK(0) << "should not get here";
    return false;
  }

  // Splits the node 'node', returning the newly created right-sibling
  // internal node 'new_inode'.
  //
  // Locking conditions:
  //   Precondition:
  //     node is locked
  //   Postcondition:
  //     node is still locked and marked SPLITTING
  //     new_inode is locked and marked INSERTING
  
  InternalNode<Traits> *SplitInternalNode(InternalNode<Traits> *node,
                                          faststring *separator_key) {
    DCHECK(node->IsLocked());
    node->SetSplitting();
    //VLOG(2) << "splitting internal node " << node->GetKey(0).ToString();

    // TODO: simplified implementation doesn't deal with splitting
    // when there are very small internal nodes.
    CHECK_GT(node->key_count(), 2)
      << "TODO: currently only support splitting nodes with >2 keys";

    // TODO: can we share code better between the node types here?
    // perhaps by making this part of NodeBase, wrapping the K,V slice pair
    // in a struct type, etc?

    // Pick the split point. The split point is the key which
    // will be moved up into the parent node.
    int split_point = node->key_count() / 2;
    Slice sep_slice = node->GetKey(split_point);
    DCHECK_GT(sep_slice.size(), 0) <<
      "got bad split key when splitting: " << node->ToString();

    separator_key->assign_copy(sep_slice.data(), sep_slice.size());

    // Example split:
    //     [ 0,   1,  2 ]
    //    /   |    |   \         .
    //  [A]  [B]  [C]  [D]
    //
    // split_point = 3/2 = 1
    // separator_key = 1
    //
    // =====>
    //
    //              [ 1 ]
    //             /    |
    //     [ 0 ]      [ 2 ]
    //    /   |        |   \     .
    //  [A]  [B]      [C]  [D]
    //

    NodePtr<Traits> separator_ptr;

    InternalNode<Traits> *new_inode = new InternalNode<Traits>(
      node->GetKey(split_point + 1),
      node->child_pointers_[split_point + 1],
      node->child_pointers_[split_point + 2]);
    // The new inode is constructed in locked and INSERTING state.

    // Copy entries to the new right-hand node.
    for (int i = split_point + 2; i < node->key_count(); i++) {
      Slice k = node->GetKey(i);
      DCHECK_GT(k.size(), 0);
      NodePtr<Traits> child = node->child_pointers_[i + 1];
      DCHECK(!child.is_null());

      // TODO: this could be done more efficiently since we know that
      // these inserts are coming in sorted order.
      CHECK_EQ(INSERT_SUCCESS, new_inode->Insert(k, child));
    }

    // Truncate the left node to remove the keys which have been
    // moved to the right node
    node->TruncateAndCompact(split_point);
    return new_inode;
  }

  // Split the given leaf node 'node', creating a new node
  // with the higher half of the elements.
  //
  // N.B: the new node is initially locked.
  void SplitLeafNode(LeafNode<Traits> *node,
                     LeafNode<Traits> **new_node) {
    DCHECK(node->IsLocked());
    DCHECK(node->version_ & VersionField::BTREE_SPLITTING_MASK);

    LeafNode<Traits> *new_leaf = new LeafNode<Traits>(true);
    new_leaf->next_ = node->next_;
    node->next_ = new_leaf;

    // Copy half the keys from node into the new leaf
    int copy_start = node->num_entries() / 2;
    CHECK_GT(copy_start, 0) <<
      "Trying to split a node with 0 or 1 entries";

    for (int i = copy_start; i < node->num_entries(); i++) {
      Slice k, v;
      node->Get(i, &k, &v);

      // TODO: this could be done more efficiently since we know that
      // these inserts are coming in sorted order.
      CHECK_EQ(INSERT_SUCCESS, new_leaf->Insert(k, v));
    }

    // Truncate the left node to remove the keys which have been
    // moved to the right node
    node->TruncateAndCompact(copy_start);
    *new_node = new_leaf;
  }


  // Splits a leaf node which is full, adding the new sibling
  // node to the tree.
  // This recurses upward splitting internal nodes as necessary.
  // The node should be locked on entrance to the function
  // and will be unlocked upon exit.
  bool SplitLeafAndInsertUp(LeafNode<Traits> *node,
                            const Slice &key,
                            const Slice &val) {
    // Leaf node should already be locked at this point
    DCHECK(node->IsLocked());
    node->SetSplitting();

    //DebugPrint();

    LeafNode<Traits> *new_leaf;
    SplitLeafNode(node, &new_leaf);

    // The new leaf node is returned still locked.
    DCHECK(new_leaf->IsLocked());
    new_leaf->SetInserting();

    // Insert the key that we were originally trying to insert in the
    // correct side post-split.
    Slice split_key = new_leaf->GetKey(0);
    LeafNode<Traits> *dst_leaf = (key.compare(split_key) < 0) ? node : new_leaf;

    CHECK_EQ(INSERT_SUCCESS, dst_leaf->Insert(key, val))
      << "TODO: node split at " << split_key.ToString()
      << " did not result in enough space for key " << key.ToString()
      << " in left node";

    // Insert the new node into the parents.
    PropagateSplitUpward(node, new_leaf, split_key);

    // NB: No ned to unlock nodes here, since it is done by the upward
    // propagation path ('ascend' label in Figure 5 in the masstree paper)

    return true;
  }

  // Assign the parent pointer of 'right', and insert it into the tree
  // by propagating splits upward.
  // Locking:
  // Precondition:
  //   left and right are both locked
  //   left is marked SPLITTING
  // Postcondition:
  //   parent is non-null
  //   parent is marked INSERTING
  //   left and right are unlocked
  void PropagateSplitUpward(NodePtr<Traits> left_ptr, NodePtr<Traits> right_ptr,
                            const Slice &split_key) {
    NodeBase<Traits> *left = left_ptr.base_ptr();
    NodeBase<Traits> *right = right_ptr.base_ptr();

    DCHECK(left->IsLocked());
    DCHECK(right->IsLocked());

    InternalNode<Traits> *parent = left->GetLockedParent();
    if (parent == NULL) {
      // Node is the root - make new parent node
      parent = new InternalNode<Traits>(split_key, left_ptr, right_ptr);
      // Constructor also reassigns parents.
      // root_ will be updated lazily by next traverser
      left->Unlock();
      right->Unlock();
      parent->Unlock();
      return;
    }

    // Parent exists. Try to insert
    parent->SetInserting();
    switch (parent->Insert(split_key, right_ptr)) {
      case INSERT_SUCCESS:
      {
        VLOG(3) << "Inserted new entry into internal node "
                << parent << " for " << split_key.ToString();
        left->Unlock();
        right->Unlock();
        parent->Unlock();
        return;
      }
      case INSERT_FULL:
      {
        // Split the node in two
        faststring sep_key(0);
        InternalNode<Traits> *new_inode = SplitInternalNode(parent, &sep_key);

        DCHECK(new_inode->IsLocked());
        DCHECK(parent->IsLocked()) << "original should still be locked";

        // Insert the new entry into the appropriate half.
        Slice inode_split(sep_key);
        InternalNode<Traits> *dst_inode =
          (split_key.compare(inode_split) < 0) ? parent : new_inode;

        VLOG(2) << "Split internal node " << parent << " for insert of "
                << split_key.ToString() << "[" << right << "]"
                << " (split at " << inode_split.ToString() << ")";

        CHECK_EQ(INSERT_SUCCESS, dst_inode->Insert(split_key, right_ptr));

        left->Unlock();
        right->Unlock();
        PropagateSplitUpward(parent, new_inode, inode_split);
        break;
      }
      default:
        CHECK(0);
    }
  }

  NodePtr<Traits> root_;
};

} // namespace btree
} // namespace tablet
} // namespace kudu

#endif
