// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "blobstore.h"

#include <bitmap/raw-bitmap.h>
#include <merkle/digest.h>
#include <mxtl/algorithm.h>
#include <mxtl/macros.h>
#include <mxtl/ref_counted.h>
#include <mxtl/ref_ptr.h>
#include <mxtl/unique_ptr.h>

#include <fs/vfs.h>

namespace blobstore {

class Blobstore;
class VnodeBlob;

typedef uint32_t BlobFlags;

// After Open;
constexpr BlobFlags kBlobStateEmpty       = 0x00000000; // Not yet allocated
// After Ioctl configuring size:
constexpr BlobFlags kBlobStateMerkleWrite = 0x00000001; // Merkle tree is being written
constexpr BlobFlags kBlobStateDataWrite   = 0x00000002; // Data is being written
// After Writing:
constexpr BlobFlags kBlobStateReadable    = 0x00000004; // Readable
// After Unlink:
constexpr BlobFlags kBlobStateReleasing   = 0x00000008; // In the process of unlinking
// Unrecoverable error state:
constexpr BlobFlags kBlobStateError       = 0x00000010; // Unrecoverable error state
constexpr BlobFlags kBlobStateMask        = 0x000000FF;
// Informational non-state flags:
constexpr BlobFlags kBlobFlagSync         = 0x00000100; // The blob is being written to disk
constexpr BlobFlags kBlobFlagDeletable    = 0x00000200; // This node should be unlinked when closed

class Blob : public mxtl::DoublyLinkedListable<mxtl::RefPtr<Blob>>,
             public mxtl::RefCounted<Blob> {
public:
    // Intrusive methods and structures
    using WAVLTreeNodeState = mxtl::WAVLTreeNodeState<mxtl::RefPtr<Blob>>;
    using NodeState = mxtl::DoublyLinkedListNodeState<mxtl::RefPtr<Blob>>;
    struct TypeListTraits {
        static NodeState& node_state(Blob& b) { return b.type_list_state_; }
    };
    struct TypeWavlTraits {
        static WAVLTreeNodeState& node_state(Blob& b) { return b.type_wavl_state_; }
    };
    const uint8_t* GetKey() const {
        return &digest_[0];
    };

    BlobFlags GetState() const {
        return flags_ & kBlobStateMask;
    }

    bool DeletionQueued() const {
        return flags_ & kBlobFlagDeletable;
    }

    void SetState(BlobFlags new_state) {
        flags_ = (flags_ & ~kBlobStateMask) | new_state;
    }

    size_t GetMapIndex() const {
        return map_index_;
    }

    void SetMapIndex(size_t i) {
        map_index_ = i;
    }

    // If successful, allocates Blob Node and Blocks (in-memory)
    // kBlobStateEmpty --> kBlobStateMerkleWrite
    mx_status_t SpaceAllocate(uint64_t size_data);

    // Writes to either the Merkle Tree or the Data section,
    // depending on the state.
    mx_status_t Write(const void* data, size_t len, size_t* actual);

    void QueueUnlink();

    mx_status_t Sync();

    // Returns a handle to an event which will be signalled when
    // the blob is readable.
    //
    // Returns "NO_ERROR" if blob is already readable.
    // Otherwise, returns size of the handle.
    mx_status_t GetReadableEvent(mx_handle_t* out);

    // Reads from a blob.
    // Requires: kBlobStateReadable
    mx_status_t Read(void* data, size_t len, size_t off, size_t* actual);

    // Creates an emtpy Blob with the given name.
    // Initializes to kBlobStateEmpty
    static mxtl::RefPtr<Blob> Create(const merkle::Digest& digest);

    uint64_t SizeData() const;

    virtual ~Blob();
    VnodeBlob* vn;

private:
    friend struct TypeListTraits;
    friend struct TypeWavlTraits;

    DISALLOW_COPY_ASSIGN_AND_MOVE(Blob);
    Blob(const merkle::Digest& digest);
    void BlobCloseHandles();

    // Read both VMOs into memory, if we haven't already.
    //
    // TODO(smklein): When we have can register the Blob Store as a pager
    // service, and it can properly handle pages faults on a vnode's contents,
    // then we can avoid reading the entire blob up-front. Until then, read
    // the contents of a VMO into memory when it is opened.
    mx_status_t InitVmos();

    mx_status_t WriteShared(const void** data, size_t* len, size_t* actual,
                            uint64_t maxlen, mx_handle_t vmo, uint64_t start_block);

    // Called by Blob once the last write has completed, updating the
    // on-disk metadata.
    mx_status_t WriteMetadata();

    NodeState type_list_state_;
    WAVLTreeNodeState type_wavl_state_;

    mx_handle_t vmo_merkle_tree_;
    uintptr_t   vmo_merkle_tree_addr_;
    mx_handle_t vmo_blob_;
    uintptr_t   vmo_blob_addr_;

    mx_handle_t readable_event_;
    uint64_t bytes_written_;

    BlobFlags flags_;
    uint8_t digest_[merkle::Digest::kLength];

    size_t map_index_;
};

// We need to define this structure to allow the Blob to be indexable by a key
// which is larger than a primitive type: the keys are 'merkle::Digest::kLength'
// bytes long.
struct MerkleRootTraits {
    static const uint8_t* GetKey(const Blob& obj) { return obj.GetKey(); }
    static bool LessThan(const uint8_t* k1, const uint8_t* k2) {
        return memcmp(k1, k2, merkle::Digest::kLength) < 0;
    }
    static bool EqualTo(const uint8_t* k1, const uint8_t* k2) {
        return memcmp(k1, k2, merkle::Digest::kLength) == 0;
    }
};

class Blobstore : public mxtl::RefCounted<Blob> {
public:
    DISALLOW_COPY_ASSIGN_AND_MOVE(Blobstore);
    friend class Blob;

    static mx_status_t Create(int blockfd, const blobstore_info_t* info, VnodeBlob** out);
    mx_status_t Unmount();
    virtual ~Blobstore();

    // Searches for a blob by name.
    // - If a readable blob with the same name exists, return it.
    // - If a blob with the same name exists, but it is not readable,
    //   ERR_BAD_STATE is returned.
    //
    // 'out' may be null -- the same error code will be returned as if it
    // was a valid pointer.
    //
    // If 'out' is not null, then the blob's refcount is increased,
    // and it will be added to the "quick lookup" map if it was not there
    // already.
    static mx_status_t LookupBlob(mxtl::RefPtr<Blobstore> bs, const merkle::Digest& digest,
                                  VnodeBlob** out);

    // Creates a new blob in-memory, with no backing disk storage (yet).
    // If a blob with the name already exists, this function fails.
    //
    // Adds Blob to the "quick lookup" map.
    static mx_status_t NewBlob(mxtl::RefPtr<Blobstore> bs, const merkle::Digest& digest,
                               VnodeBlob** out);

    // Removes blob from 'active' hashmap.
    // Deletes the blob if requested.
    mx_status_t ReleaseBlob(mxtl::RefPtr<Blob> blob);

    int blockfd_;
    blobstore_info_t info_;
private:
    Blobstore(int fd, const blobstore_info_t* info);
    mx_status_t LoadBitmaps();

    // Acquire a dummy vnode that acts like a root directory, allowing access
    // to other vnodes.
    static mx_status_t RootVnodeNew(mxtl::RefPtr<Blobstore> bs, VnodeBlob** out);

    // Wrap a blob in a new vnode.
    static mx_status_t VnodeNew(mxtl::RefPtr<Blobstore> bs, mxtl::RefPtr<Blob> blob,
                                VnodeBlob** out);

    // Finds space for a block in memory. Does not update disk.
    mx_status_t AllocateBlocks(size_t nblocks, size_t* blkno_out);
    void FreeBlocks(size_t nblocks, size_t blkno);

    // Finds space for a blob node in memory. Does not update disk.
    mx_status_t AllocateNode(size_t* node_index_out);
    void FreeNode(size_t node_index);

    // Access the nth block of the block bitmap.
    void* GetBlockmapData(uint64_t n) const;
    // Access the nth block of the node map.
    void* GetNodemapData(uint64_t n) const;

    // Given a contiguous number of blocks after a starting block,
    // write out the bitmap to disk for the corresponding blocks.
    mx_status_t WriteBitmap(uint64_t nblocks, uint64_t start_block);

    // Given a node within the node map at an index, write it to disk.
    mx_status_t WriteNode(size_t map_index);

    using WAVLTreeByMerkle = mxtl::WAVLTree<const uint8_t*,
                                            mxtl::RefPtr<Blob>,
                                            MerkleRootTraits,
                                            Blob::TypeWavlTraits>;
    WAVLTreeByMerkle hash_; // Map of all 'in use' blobs

    RawBitmap block_map_;
    mxtl::unique_ptr<blobstore_inode_t[]> node_map_;
};

int blobstore_mkfs(int fd);

mx_status_t blobstore_mount(VnodeBlob** out, int blockfd);

mx_status_t readblk(int fd, uint64_t bno, void* data);
mx_status_t writeblk(int fd, uint64_t bno, const void* data);

class VnodeBlob final : public fs::Vnode {
public:
    VnodeBlob(mxtl::RefPtr<Blob> b, mxtl::RefPtr<Blobstore> bs) :
        blobstore(bs), blob(b) {}

    bool IsDirectory() const { return blob == nullptr; }

    ssize_t Ioctl(uint32_t op, const void* in_buf, size_t in_len,
                  void* out_buf, size_t out_len) final;

    const mxtl::RefPtr<Blobstore> blobstore;
    const mxtl::RefPtr<Blob> blob;

private:
    mx_status_t GetHandles(uint32_t flags, mx_handle_t* hnds,
                           uint32_t* type, void* extra, uint32_t* esize) final;
    void Release() final;
    mx_status_t Open(uint32_t flags) final;
    mx_status_t Close() final;
    ssize_t Read(void* data, size_t len, size_t off) final;
    ssize_t Write(const void* data, size_t len, size_t off) final;
    mx_status_t Lookup(fs::Vnode** out, const char* name, size_t len) final;
    mx_status_t Getattr(vnattr_t* a) final;
    mx_status_t Create(fs::Vnode** out, const char* name, size_t len, uint32_t mode) final;
    mx_status_t Unlink(const char* name, size_t len, bool must_be_dir) final;
    mx_status_t Sync() final;
};

} // namespace blobstore
