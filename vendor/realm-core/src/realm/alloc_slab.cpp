/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include <cinttypes>
#include <type_traits>
#include <exception>
#include <algorithm>
#include <memory>
#include <mutex>
#include <map>
#include <atomic>

#ifdef REALM_DEBUG
#include <iostream>
#include <unordered_set>
#endif

#ifdef REALM_SLAB_ALLOC_DEBUG
#include <cstdlib>
#endif

#include <realm/util/encrypted_file_mapping.hpp>
#include <realm/util/miscellaneous.hpp>
#include <realm/util/terminate.hpp>
#include <realm/util/thread.hpp>
#include <realm/util/scope_exit.hpp>
#include <realm/array.hpp>
#include <realm/alloc_slab.hpp>

using namespace realm;
using namespace realm::util;


namespace {

#ifdef REALM_SLAB_ALLOC_DEBUG
std::map<ref_type, void*> malloc_debug_map;
#endif

class InvalidFreeSpace : std::exception {
public:
    const char* what() const noexcept override
    {
        return "Free space tracking was lost due to out-of-memory";
    }
};

std::atomic<size_t> total_slab_allocated(0);

} // anonymous namespace

size_t SlabAlloc::get_total_slab_size() noexcept
{
    return total_slab_allocated;
}

SlabAlloc::SlabAlloc()
{
    m_initial_section_size = 1UL << section_shift; // page_size();
    m_free_space_state = free_space_Clean;
    m_baseline = 0;
}

util::File& SlabAlloc::get_file()
{
    return m_file;
}


const SlabAlloc::Header SlabAlloc::empty_file_header = {
    {0, 0}, // top-refs
    {'T', '-', 'D', 'B'},
    {0, 0}, // undecided file format
    0,      // reserved
    0       // flags (lsb is select bit)
};


void SlabAlloc::init_streaming_header(Header* streaming_header, int file_format_version)
{
    using storage_type = std::remove_reference<decltype(Header::m_file_format[0])>::type;
    REALM_ASSERT(!util::int_cast_has_overflow<storage_type>(file_format_version));
    *streaming_header = {
        {0xFFFFFFFFFFFFFFFFULL, 0}, // top-refs
        {'T', '-', 'D', 'B'},
        {storage_type(file_format_version), 0},
        0, // reserved
        0  // flags (lsb is select bit)
    };
}

inline SlabAlloc::Slab::Slab(ref_type r, size_t s)
    : ref_end(r)
    , size(s)
{
    total_slab_allocated.fetch_add(s, std::memory_order_relaxed);
    addr = static_cast<char*>(util::mmap_anon(size));
#if REALM_ENABLE_ALLOC_SET_ZERO
    std::fill(addr, addr + size, 0);
#endif
}

SlabAlloc::Slab::~Slab()
{
    total_slab_allocated.fetch_sub(size, std::memory_order_relaxed);
    if (addr)
        util::munmap(addr, size);
}

void SlabAlloc::detach() noexcept
{
    delete[] m_ref_translation_ptr;
    m_ref_translation_ptr.store(nullptr);
    m_translation_table_size = 0;
    set_read_only(true);
    purge_old_mappings(static_cast<uint64_t>(-1), 0);
    switch (m_attach_mode) {
        case attach_None:
            break;
        case attach_UsersBuffer:
            break;
        case attach_OwnedBuffer:
            delete[] m_data;
            break;
        case attach_SharedFile:
        case attach_UnsharedFile:
            m_data = 0;
            m_mappings.clear();
            m_youngest_live_version = 0;
            m_file.close();
            break;
        default:
            REALM_UNREACHABLE();
    }

    // Release all allocated memory - this forces us to create new
    // slabs after re-attaching thereby ensuring that the slabs are
    // placed correctly (logically) after the end of the file.
    m_slabs.clear();
    clear_freelists();
#if REALM_ENABLE_ENCRYPTION
    m_realm_file_info = nullptr;
#endif

    m_attach_mode = attach_None;
}


SlabAlloc::~SlabAlloc() noexcept
{
#ifdef REALM_DEBUG
    if (is_attached()) {
        // A shared group does not guarantee that all space is free
        if (m_attach_mode != attach_SharedFile) {
            // No point inchecking if free space info is invalid
            if (m_free_space_state != free_space_Invalid) {
                if (REALM_COVER_NEVER(!is_all_free())) {
                    print();
#ifndef REALM_SLAB_ALLOC_DEBUG
                    std::cerr << "To get the stack-traces of the corresponding allocations,"
                                 "first compile with REALM_SLAB_ALLOC_DEBUG defined,"
                                 "then run under Valgrind with --leak-check=full\n";
                    REALM_TERMINATE("SlabAlloc detected a leak");
#endif
                }
            }
        }
    }
#endif

    if (is_attached())
        detach();
}


MemRef SlabAlloc::do_alloc(size_t size)
{
    CriticalSection cs(changes);
    REALM_ASSERT_EX(0 < size, size, get_file_path_for_assertions());
    REALM_ASSERT_EX((size & 0x7) == 0, size,
                    get_file_path_for_assertions()); // only allow sizes that are multiples of 8
    REALM_ASSERT_EX(is_attached(), get_file_path_for_assertions());
    // This limits the size of any array to ensure it can fit within a memory section.
    // NOTE: This limit is lower than the limit set by the encoding in node_header.hpp
    REALM_ASSERT_RELEASE_EX(size < (1 << section_shift), size, get_file_path_for_assertions());

    // If we failed to correctly record free space, new allocations cannot be
    // carried out until the free space record is reset.
    if (REALM_COVER_NEVER(m_free_space_state == free_space_Invalid))
        throw InvalidFreeSpace();

    m_free_space_state = free_space_Dirty;
    m_commit_size += size;

    // minimal allocation is sizeof(FreeListEntry)
    if (size < sizeof(FreeBlock))
        size = sizeof(FreeBlock);
    // align to multipla of 8
    if (size & 0x7)
        size = (size + 7) & ~0x7;

    FreeBlock* entry = allocate_block(static_cast<int>(size));
    mark_allocated(entry);
    ref_type ref = entry->ref;

#ifdef REALM_DEBUG
    if (REALM_COVER_NEVER(m_debug_out))
        std::cerr << "Alloc ref: " << ref << " size: " << size << "\n";
#endif

    char* addr = reinterpret_cast<char*>(entry);
    REALM_ASSERT_EX(addr == translate(ref), addr, ref, get_file_path_for_assertions());

#if REALM_ENABLE_ALLOC_SET_ZERO
    std::fill(addr, addr + size, 0);
#endif
#ifdef REALM_SLAB_ALLOC_DEBUG
    malloc_debug_map[ref] = malloc(1);
#endif
    REALM_ASSERT_EX(ref >= m_baseline, ref, m_baseline, get_file_path_for_assertions());
    return MemRef(addr, ref, *this);
}

SlabAlloc::FreeBlock* SlabAlloc::get_prev_block_if_mergeable(SlabAlloc::FreeBlock* entry)
{
    auto bb = bb_before(entry);
    if (bb->block_before_size <= 0)
        return nullptr; // no prev block, or it is in use
    return block_before(bb);
}

SlabAlloc::FreeBlock* SlabAlloc::get_next_block_if_mergeable(SlabAlloc::FreeBlock* entry)
{
    auto bb = bb_after(entry);
    if (bb->block_after_size <= 0)
        return nullptr; // no next block, or it is in use
    return block_after(bb);
}

SlabAlloc::FreeList SlabAlloc::find(int size)
{
    FreeList retval;
    retval.it = m_block_map.lower_bound(size);
    if (retval.it != m_block_map.end()) {
        retval.size = retval.it->first;
    }
    else {
        retval.size = 0;
    }
    return retval;
}

SlabAlloc::FreeList SlabAlloc::find_larger(FreeList hint, int size)
{
    int needed_size = size + sizeof(BetweenBlocks) + sizeof(FreeBlock);
    while (hint.it != m_block_map.end() && hint.it->first < needed_size)
        ++hint.it;
    if (hint.it == m_block_map.end())
        hint.size = 0; // indicate "not found"
    return hint;
}

SlabAlloc::FreeBlock* SlabAlloc::pop_freelist_entry(FreeList list)
{
    FreeBlock* retval = list.it->second;
    FreeBlock* header = retval->next;
    if (header == retval)
        m_block_map.erase(list.it);
    else
        list.it->second = header;
    retval->unlink();
    return retval;
}

void SlabAlloc::FreeBlock::unlink()
{
    auto _next = next;
    auto _prev = prev;
    _next->prev = prev;
    _prev->next = next;
    clear_links();
}

void SlabAlloc::remove_freelist_entry(FreeBlock* entry)
{
    int size = bb_before(entry)->block_after_size;
    auto it = m_block_map.find(size);
    REALM_ASSERT_EX(it != m_block_map.end(), get_file_path_for_assertions());
    auto header = it->second;
    if (header == entry) {
        header = entry->next;
        if (header == entry)
            m_block_map.erase(it);
        else
            it->second = header;
    }
    entry->unlink();
}

void SlabAlloc::push_freelist_entry(FreeBlock* entry)
{
    int size = bb_before(entry)->block_after_size;
    FreeBlock* header;
    auto it = m_block_map.find(size);
    if (it != m_block_map.end()) {
        header = it->second;
        it->second = entry;
        entry->next = header;
        entry->prev = header->prev;
        entry->prev->next = entry;
        entry->next->prev = entry;
    }
    else {
        header = nullptr;
        m_block_map[size] = entry;
        entry->next = entry->prev = entry;
    }
}

void SlabAlloc::mark_freed(FreeBlock* entry, int size)
{
    auto bb = bb_before(entry);
    REALM_ASSERT_EX(bb->block_after_size < 0, bb->block_after_size, get_file_path_for_assertions());
    auto alloc_size = -bb->block_after_size;
    int max_waste = sizeof(FreeBlock) + sizeof(BetweenBlocks);
    REALM_ASSERT_EX(alloc_size >= size && alloc_size <= size + max_waste, alloc_size, size,
                    get_file_path_for_assertions());
    bb->block_after_size = alloc_size;
    bb = bb_after(entry);
    REALM_ASSERT_EX(bb->block_before_size < 0, bb->block_before_size, get_file_path_for_assertions());
    REALM_ASSERT(-bb->block_before_size == alloc_size);
    bb->block_before_size = alloc_size;
}

void SlabAlloc::mark_allocated(FreeBlock* entry)
{
    auto bb = bb_before(entry);
    REALM_ASSERT_EX(bb->block_after_size > 0, bb->block_after_size, get_file_path_for_assertions());
    auto bb2 = bb_after(entry);
    bb->block_after_size = 0 - bb->block_after_size;
    REALM_ASSERT_EX(bb2->block_before_size > 0, bb2->block_before_size, get_file_path_for_assertions());
    bb2->block_before_size = 0 - bb2->block_before_size;
}

SlabAlloc::FreeBlock* SlabAlloc::allocate_block(int size)
{
    FreeList list = find(size);
    if (list.found_exact(size)) {
        return pop_freelist_entry(list);
    }
    // no exact matches.
    list = find_larger(list, size);
    FreeBlock* block;
    if (list.found_something()) {
        block = pop_freelist_entry(list);
    }
    else {
        block = grow_slab(size);
    }
    FreeBlock* remaining = break_block(block, size);
    if (remaining)
        push_freelist_entry(remaining);
    REALM_ASSERT_EX(size_from_block(block) >= size, size_from_block(block), size, get_file_path_for_assertions());
    return block;
}

SlabAlloc::FreeBlock* SlabAlloc::slab_to_entry(const Slab& slab, ref_type ref_start)
{
    auto bb = reinterpret_cast<BetweenBlocks*>(slab.addr);
    bb->block_before_size = 0;
    int block_size = static_cast<int>(slab.ref_end - ref_start - 2 * sizeof(BetweenBlocks));
    bb->block_after_size = block_size;
    auto entry = block_after(bb);
    entry->clear_links();
    entry->ref = ref_start + sizeof(BetweenBlocks);
    bb = bb_after(entry);
    bb->block_before_size = block_size;
    bb->block_after_size = 0;
    return entry;
}

void SlabAlloc::clear_freelists()
{
    m_block_map.clear();
}

void SlabAlloc::rebuild_freelists_from_slab()
{
    clear_freelists();
    ref_type ref_start = align_size_to_section_boundary(m_baseline.load(std::memory_order_relaxed));
    for (const auto& e : m_slabs) {
        FreeBlock* entry = slab_to_entry(e, ref_start);
        push_freelist_entry(entry);
        ref_start = align_size_to_section_boundary(e.ref_end);
    }
}

SlabAlloc::FreeBlock* SlabAlloc::break_block(FreeBlock* block, int new_size)
{
    int size = size_from_block(block);
    int remaining_size = size - (new_size + sizeof(BetweenBlocks));
    if (remaining_size < static_cast<int>(sizeof(FreeBlock)))
        return nullptr;
    bb_after(block)->block_before_size = remaining_size;
    bb_before(block)->block_after_size = new_size;
    auto bb_between = bb_after(block);
    bb_between->block_before_size = new_size;
    bb_between->block_after_size = remaining_size;
    FreeBlock* remaining_block = block_after(bb_between);
    remaining_block->ref = block->ref + new_size + sizeof(BetweenBlocks);
    remaining_block->clear_links();
    block->clear_links();
    return remaining_block;
}

SlabAlloc::FreeBlock* SlabAlloc::merge_blocks(FreeBlock* first, FreeBlock* last)
{
    int size_first = size_from_block(first);
    int size_last = size_from_block(last);
    int new_size = size_first + size_last + sizeof(BetweenBlocks);
    bb_before(first)->block_after_size = new_size;
    bb_after(last)->block_before_size = new_size;
    return first;
}

SlabAlloc::FreeBlock* SlabAlloc::grow_slab(int size)
{
    // Allocate new slab.
    // - Always allocate at least 128K. This is also the amount of
    //   memory that we allow the slab allocator to keep between
    //   transactions. Allowing it to keep a small amount between
    //   transactions makes very small transactions faster by avoiding
    //   repeated unmap/mmap system calls.
    // - When allocating, allocate as much as we already have, but
    // - Never allocate more than a full section (64MB). This policy
    //   leads to gradual allocation of larger and larger blocks until
    //   we reach allocation of entire sections.
    size += 2 * sizeof(BetweenBlocks);
    size_t new_size = minimal_alloc;
    while (new_size < uint64_t(size))
        new_size += minimal_alloc;
    size_t already_allocated = get_allocated_size();
    if (new_size < already_allocated)
        new_size = already_allocated;
    if (new_size > maximal_alloc)
        new_size = maximal_alloc;

    ref_type ref;
    if (m_slabs.empty()) {
        ref = m_baseline.load(std::memory_order_relaxed);
    }
    else {
        // Find size of memory that has been modified (through copy-on-write) in current write transaction
        ref_type curr_ref_end = to_size_t(m_slabs.back().ref_end);
        REALM_ASSERT_DEBUG_EX(curr_ref_end >= m_baseline, curr_ref_end, m_baseline, get_file_path_for_assertions());
        ref = curr_ref_end;
    }
    ref = align_size_to_section_boundary(ref);
    size_t ref_end = ref;
    if (REALM_UNLIKELY(int_add_with_overflow_detect(ref_end, new_size))) {
        throw MaximumFileSizeExceeded("AllocSlab slab ref_end size overflow: " + util::to_string(ref) + " + " +
                                      util::to_string(new_size));
    }

    REALM_ASSERT(matches_section_boundary(ref));

    std::lock_guard<std::mutex> lock(m_mapping_mutex);
    // Create new slab and add to list of slabs
    m_slabs.emplace_back(ref_end, new_size); // Throws
    const Slab& slab = m_slabs.back();
    extend_fast_mapping_with_slab(slab.addr);

    // build a single block from that entry
    return slab_to_entry(slab, ref);
}


void SlabAlloc::do_free(ref_type ref, char* addr)
{
    REALM_ASSERT_EX(translate(ref) == addr, translate(ref), addr, get_file_path_for_assertions());
    CriticalSection cs(changes);

    bool read_only = is_read_only(ref);
#ifdef REALM_SLAB_ALLOC_DEBUG
    free(malloc_debug_map[ref]);
#endif

    // Get size from segment
    size_t size =
        read_only ? NodeHeader::get_byte_size_from_header(addr) : NodeHeader::get_capacity_from_header(addr);

#ifdef REALM_DEBUG
    if (REALM_COVER_NEVER(m_debug_out))
        std::cerr << "Free ref: " << ref << " size: " << size << "\n";
#endif

    if (REALM_COVER_NEVER(m_free_space_state == free_space_Invalid))
        return;

    // Mutable memory cannot be freed unless it has first been allocated, and
    // any allocation puts free space tracking into the "dirty" state.
    REALM_ASSERT_EX(read_only || m_free_space_state == free_space_Dirty, read_only, m_free_space_state,
                    free_space_Dirty, get_file_path_for_assertions());

    m_free_space_state = free_space_Dirty;

    if (read_only) {
        // Free space in read only segment is tracked separately
        try {
            REALM_ASSERT_RELEASE_EX(ref != 0, ref, get_file_path_for_assertions());
            REALM_ASSERT_RELEASE_EX(!(ref & 7), ref, get_file_path_for_assertions());
            auto next = m_free_read_only.lower_bound(ref);
            if (next != m_free_read_only.end()) {
                REALM_ASSERT_RELEASE_EX(ref + size <= next->first, ref, size, next->first, next->second,
                                        get_file_path_for_assertions());
                // See if element can be combined with next element
                if (ref + size == next->first) {
                    // if so, combine to include next element and remove that from collection
                    size += next->second;
                    next = m_free_read_only.erase(next);
                }
            }
            if (!m_free_read_only.empty() && next != m_free_read_only.begin()) {
                // There must be a previous element - see if we can merge
                auto prev = next;
                prev--;

                REALM_ASSERT_RELEASE_EX(prev->first + prev->second <= ref, ref, size, prev->first, prev->second,
                                        get_file_path_for_assertions());
                // See if element can be combined with previous element
                // We can do that just by adding the size
                if (prev->first + prev->second == ref) {
                    prev->second += size;
                    return; // Done!
                }
                m_free_read_only.emplace_hint(next, ref, size); // Throws
            }
            else {
                m_free_read_only.emplace(ref, size); // Throws
            }
        }
        catch (...) {
            m_free_space_state = free_space_Invalid;
        }
    }
    else {
        m_commit_size -= size;

        // fixup size to take into account the allocator's need to store a FreeBlock in a freed block
        if (size < sizeof(FreeBlock))
            size = sizeof(FreeBlock);
        // align to multipla of 8
        if (size & 0x7)
            size = (size + 7) & ~0x7;

        FreeBlock* e = reinterpret_cast<FreeBlock*>(addr);
        REALM_ASSERT_RELEASE_EX(size < 2UL * 1024 * 1024 * 1024, size, get_file_path_for_assertions());
        mark_freed(e, static_cast<int>(size));
        free_block(ref, e);
    }
}

void SlabAlloc::free_block(ref_type ref, SlabAlloc::FreeBlock* block)
{
    // merge with surrounding blocks if possible
    block->ref = ref;
    FreeBlock* prev = get_prev_block_if_mergeable(block);
    if (prev) {
        remove_freelist_entry(prev);
        block = merge_blocks(prev, block);
    }
    FreeBlock* next = get_next_block_if_mergeable(block);
    if (next) {
        remove_freelist_entry(next);
        block = merge_blocks(block, next);
    }
    push_freelist_entry(block);
}

size_t SlabAlloc::consolidate_free_read_only()
{
    CriticalSection cs(changes);
    if (REALM_COVER_NEVER(m_free_space_state == free_space_Invalid))
        throw InvalidFreeSpace();

    return m_free_read_only.size();
}


MemRef SlabAlloc::do_realloc(size_t ref, char* addr, size_t old_size, size_t new_size)
{
    REALM_ASSERT_DEBUG(translate(ref) == addr);
    REALM_ASSERT_EX(0 < new_size, new_size, get_file_path_for_assertions());
    REALM_ASSERT_EX((new_size & 0x7) == 0, new_size,
                    get_file_path_for_assertions()); // only allow sizes that are multiples of 8

    // Possible future enhancement: check if we can extend current space instead
    // of unconditionally allocating new space. In that case, remember to
    // check whether m_free_space_state == free_state_Invalid. Also remember to
    // fill with zero if REALM_ENABLE_ALLOC_SET_ZERO is non-zero.

    // Allocate new space
    MemRef new_mem = do_alloc(new_size); // Throws

    // Copy existing segment
    char* new_addr = new_mem.get_addr();
    realm::safe_copy_n(addr, old_size, new_addr);

    // Add old segment to freelist
    do_free(ref, addr);

#ifdef REALM_DEBUG
    if (REALM_COVER_NEVER(m_debug_out)) {
        std::cerr << "Realloc orig_ref: " << ref << " old_size: " << old_size << " new_ref: " << new_mem.get_ref()
                  << " new_size: " << new_size << "\n";
    }
#endif // REALM_DEBUG

    return new_mem;
}


char* SlabAlloc::do_translate(ref_type) const noexcept
{
    REALM_ASSERT(false); // never come here
    return nullptr;
}


int SlabAlloc::get_committed_file_format_version() const noexcept
{
    if (m_mappings.size()) {
        // if we have mapped a file, m_mappings will have at least one mapping and
        // the first will be to the start of the file. Don't come here, if we're
        // just attaching a buffer. They don't have mappings.
        realm::util::encryption_read_barrier(m_mappings[0].primary_mapping, 0, sizeof(Header));
    }

    const Header& header = *reinterpret_cast<const Header*>(m_data);
    int slot_selector = ((header.m_flags & SlabAlloc::flags_SelectBit) != 0 ? 1 : 0);
    int file_format_version = int(header.m_file_format[slot_selector]);
    return file_format_version;
}

bool SlabAlloc::is_file_on_streaming_form(const Header& header)
{
    // LIMITATION: Only come here if we've already had a read barrier for the affected part of the file
    int slot_selector = ((header.m_flags & SlabAlloc::flags_SelectBit) != 0 ? 1 : 0);
    uint_fast64_t ref = uint_fast64_t(header.m_top_ref[slot_selector]);
    return (slot_selector == 0 && ref == 0xFFFFFFFFFFFFFFFFULL);
}

ref_type SlabAlloc::get_top_ref(const char* buffer, size_t len)
{
    // LIMITATION: Only come here if we've already had a read barrier for the affected part of the file
    const Header& header = reinterpret_cast<const Header&>(*buffer);
    int slot_selector = ((header.m_flags & SlabAlloc::flags_SelectBit) != 0 ? 1 : 0);
    if (is_file_on_streaming_form(header)) {
        const StreamingFooter& footer = *(reinterpret_cast<const StreamingFooter*>(buffer + len) - 1);
        return ref_type(footer.m_top_ref);
    }
    else {
        return to_ref(header.m_top_ref[slot_selector]);
    }
}

std::string SlabAlloc::get_file_path_for_assertions() const
{
    return m_file.get_path();
}

ref_type SlabAlloc::attach_file(const std::string& file_path, Config& cfg)
{
    m_cfg = cfg;
    // ExceptionSafety: If this function throws, it must leave the allocator in
    // the detached state.

    REALM_ASSERT_EX(!is_attached(), get_file_path_for_assertions());

    // When 'read_only' is true, this function will throw InvalidDatabase if the
    // file exists already but is empty. This can happen if another process is
    // currently creating it. Note however, that it is only legal for multiple
    // processes to access a database file concurrently if it is done via a
    // DB, and in that case 'read_only' can never be true.
    REALM_ASSERT_EX(!(cfg.is_shared && cfg.read_only), cfg.is_shared, cfg.read_only, get_file_path_for_assertions());
    // session_initiator can be set *only* if we're shared.
    REALM_ASSERT_EX(cfg.is_shared || !cfg.session_initiator, cfg.is_shared, cfg.session_initiator,
                    get_file_path_for_assertions());
    // clear_file can be set *only* if we're the first session.
    REALM_ASSERT_EX(cfg.session_initiator || !cfg.clear_file, cfg.session_initiator, cfg.clear_file,
                    get_file_path_for_assertions());

    // Create a deep copy of the file_path string, otherwise it can appear that
    // users are leaking paths because string assignment operator implementations might
    // actually be reference counting with copy-on-write. If our all_files map
    // holds onto these references (since it is still reachable memory) it can appear
    // as a leak in the user application, but it is actually us (and that's ok).
    const std::string path = file_path.c_str();

    using namespace realm::util;
    File::AccessMode access = cfg.read_only ? File::access_ReadOnly : File::access_ReadWrite;
    File::CreateMode create = cfg.read_only || cfg.no_create ? File::create_Never : File::create_Auto;
    set_read_only(cfg.read_only);
    m_file.open(path.c_str(), access, create, 0); // Throws
    auto physical_file_size = m_file.get_size();
    // Note that get_size() may (will) return a different size before and after
    // the call below to set_encryption_key.
    m_file.set_encryption_key(cfg.encryption_key);
    File::CloseGuard fcg(m_file);

    size_t size = 0;
    // The size of a database file must not exceed what can be encoded in
    // size_t.
    if (REALM_UNLIKELY(int_cast_with_overflow_detect(m_file.get_size(), size)))
        throw InvalidDatabase("Realm file too large", path);
    if (cfg.encryption_key && size == 0 && physical_file_size != 0) {
        // The opened file holds data, but is so small it cannot have
        // been created with encryption
        throw std::runtime_error("Attempt to open unencrypted file with encryption key");
    }
    if (size == 0 || cfg.clear_file) {
        if (REALM_UNLIKELY(cfg.read_only))
            throw InvalidDatabase("Read-only access to empty Realm file", path);

        const char* data = reinterpret_cast<const char*>(&empty_file_header);
        m_file.write(data, sizeof empty_file_header); // Throws

        // Pre-alloc initial space
        size_t initial_size = page_size(); // m_initial_section_size;
        m_file.prealloc(initial_size);     // Throws

        bool disable_sync = get_disable_sync_to_disk() || cfg.disable_sync;
        if (!disable_sync)
            m_file.sync(); // Throws

        size = initial_size;
    }
    ref_type top_ref;
    try {
        note_reader_start(this);
        // we'll read header and (potentially) footer
        File::Map<char> map_header(m_file, File::access_ReadOnly, sizeof(Header));
        // if file is too small we'll catch it in validate_header - but we need to prevent an invalid mapping first
        size_t footer_ref = size < (sizeof(StreamingFooter) + sizeof(Header)) ? 0 : (size - sizeof(StreamingFooter));
        size_t footer_page_base = footer_ref & ~(page_size() - 1);
        size_t footer_offset = footer_ref - footer_page_base;
        File::Map<char> map_footer(m_file, footer_page_base, File::access_ReadOnly,
                                   sizeof(StreamingFooter) + footer_offset, 0);
        realm::util::encryption_read_barrier(map_header, 0, sizeof(Header));
        realm::util::encryption_read_barrier(map_footer, footer_offset, sizeof(StreamingFooter));
        auto header = reinterpret_cast<const Header*>(map_header.get_addr());
        auto footer = reinterpret_cast<const StreamingFooter*>(map_footer.get_addr() + footer_offset);
        top_ref = validate_header(header, footer, size, path); // Throws
        m_attach_mode = cfg.is_shared ? attach_SharedFile : attach_UnsharedFile;
        m_data = map_header.get_addr(); // <-- needed below

        // Make sure the database is not on streaming format. If we did not do this,
        // a later commit would have to do it. That would require coordination with
        // anybody concurrently joining the session, so it seems easier to do it at
        // session initialization, even if it means writing the database during open.
        if (cfg.session_initiator && is_file_on_streaming_form(*header)) {
            // Don't compare file format version fields as they are allowed to differ.
            // Also don't compare reserved fields.
            REALM_ASSERT_EX(header->m_flags == 0, header->m_flags, get_file_path_for_assertions());
            REALM_ASSERT_EX(header->m_mnemonic[0] == uint8_t('T'), header->m_mnemonic[0],
                            get_file_path_for_assertions());
            REALM_ASSERT_EX(header->m_mnemonic[1] == uint8_t('-'), header->m_mnemonic[1],
                            get_file_path_for_assertions());
            REALM_ASSERT_EX(header->m_mnemonic[2] == uint8_t('D'), header->m_mnemonic[2],
                            get_file_path_for_assertions());
            REALM_ASSERT_EX(header->m_mnemonic[3] == uint8_t('B'), header->m_mnemonic[3],
                            get_file_path_for_assertions());
            REALM_ASSERT_EX(header->m_top_ref[0] == 0xFFFFFFFFFFFFFFFFULL, header->m_top_ref[0],
                            get_file_path_for_assertions());
            REALM_ASSERT_EX(header->m_top_ref[1] == 0, header->m_top_ref[1], get_file_path_for_assertions());
            REALM_ASSERT_EX(footer->m_magic_cookie == footer_magic_cookie, footer->m_magic_cookie,
                            get_file_path_for_assertions());
            {
                File::Map<Header> writable_map(m_file, File::access_ReadWrite, sizeof(Header)); // Throws
                Header& writable_header = *writable_map.get_addr();
                realm::util::encryption_read_barrier(writable_map, 0);
                writable_header.m_top_ref[1] = footer->m_top_ref;
                writable_header.m_file_format[1] = writable_header.m_file_format[0];
                realm::util::encryption_write_barrier(writable_map, 0);
                writable_map.sync();
                realm::util::encryption_read_barrier(writable_map, 0);
                writable_header.m_flags |= flags_SelectBit;
                realm::util::encryption_write_barrier(writable_map, 0);
                writable_map.sync();

                realm::util::encryption_read_barrier(map_header, 0, sizeof(Header));
            }
        }
    }
    catch (const DecryptionFailed&) {
        note_reader_end(this);
        throw InvalidDatabase("Realm file decryption failed", path);
    }
    catch (const std::exception& e) {
        note_reader_end(this);
        // we end up here if any of the file or mapping operations fail.
        throw InvalidDatabase(util::format("Realm file initial open failed: %1", e.what()), path);
    }
    catch (...) {
        note_reader_end(this);
        throw InvalidDatabase("Realm file initial open failed: unknown error", path);
    }
    m_baseline = 0;
    auto handler = [this]() noexcept {
        note_reader_end(this);
    };
    auto reader_end_guard = make_scope_exit(handler);
    // make sure that any call to begin_read cause any slab to be placed in free
    // lists correctly
    m_free_space_state = free_space_Invalid;

    // Ensure clean up, if we need to back out:
    DetachGuard dg(*this);

    // We can only safely mmap the file, if its size matches a page boundary. If not,
    // we must change the size to match before mmaping it.
    if (size != round_up_to_page_size(size)) {
        // The file size did not match a page boundary.
        // We must extend the file to a page boundary (unless already there)
        // The file must be extended to match in size prior to being mmapped,
        // as extending it after mmap has undefined behavior.
        if (cfg.read_only) {
            // If the file is opened read-only, we cannot extend it. This is not a problem,
            // because for a read-only file we assume that it will not change while we use it.
            // This assumption obviously will not hold, if the file is shared by multiple
            // processes or threads with different opening modes.
            // Currently, there is no way to detect if this assumption is violated.
            m_baseline = 0;
        }
        else {
            if (cfg.session_initiator || !cfg.is_shared) {
                // We can only safely extend the file if we're the session initiator, or if
                // the file isn't shared at all. Extending the file to a page boundary is ONLY
                // done to ensure well defined behavior for memory mappings. It does not matter,
                // that the free space management isn't informed
                size = round_up_to_page_size(size);
                m_file.prealloc(size);
                m_baseline = 0;
            }
            else {
                // Getting here, we have a file of a size that will not work, and without being
                // allowed to extend it. This should not be possible. But allowing a retry is
                // arguably better than giving up and crashing...
                throw Retry();
            }
        }
    }

    reset_free_space_tracking();
    update_reader_view(size);
    REALM_ASSERT(m_mappings.size());
    m_data = m_mappings[0].primary_mapping.get_addr();
    realm::util::encryption_read_barrier(m_mappings[0].primary_mapping, 0, sizeof(Header));
    dg.release();  // Do not detach
    fcg.release(); // Do not close
#if REALM_ENABLE_ENCRYPTION
    m_realm_file_info = util::get_file_info_for_file(m_file);
#endif
    return top_ref;
}

void SlabAlloc::note_reader_start(const void* reader_id)
{
#if REALM_ENABLE_ENCRYPTION
    if (m_realm_file_info)
        util::encryption_note_reader_start(*m_realm_file_info, reader_id);
#else
    static_cast<void>(reader_id);
#endif
}

void SlabAlloc::note_reader_end(const void* reader_id) noexcept
{
#if REALM_ENABLE_ENCRYPTION
    if (m_realm_file_info)
        util::encryption_note_reader_end(*m_realm_file_info, reader_id);
#else
    static_cast<void>(reader_id);
#endif
}

ref_type SlabAlloc::attach_buffer(const char* data, size_t size)
{
    // ExceptionSafety: If this function throws, it must leave the allocator in
    // the detached state.

    REALM_ASSERT_EX(!is_attached(), get_file_path_for_assertions());
    REALM_ASSERT_EX(size <= (1UL << section_shift), get_file_path_for_assertions());

    // Verify the data structures
    std::string path;                                     // No path
    ref_type top_ref = validate_header(data, size, path); // Throws

    m_data = data;
    size = align_size_to_section_boundary(size);
    m_baseline = size;
    m_attach_mode = attach_UsersBuffer;

    m_translation_table_size = 1;
    m_ref_translation_ptr = new RefTranslation[1]{RefTranslation{const_cast<char*>(m_data)}};
    return top_ref;
}


void SlabAlloc::attach_empty()
{
    // ExceptionSafety: If this function throws, it must leave the allocator in
    // the detached state.

    REALM_ASSERT_EX(!is_attached(), get_file_path_for_assertions());

    m_attach_mode = attach_OwnedBuffer;
    m_data = nullptr; // Empty buffer

    // Below this point (assignment to `m_attach_mode`), nothing must throw.

    // No ref must ever be less than the header size, so we will use that as the
    // baseline here.
    size_t size = align_size_to_section_boundary(sizeof(Header));
    m_baseline = size;
    m_translation_table_size = 1;
    m_ref_translation_ptr = new RefTranslation[1];
}

void SlabAlloc::throw_header_exception(std::string msg, const Header& header, const std::string& path)
{
    char buf[256];
    sprintf(buf,
            ". top_ref[0]: %" PRIX64 ", top_ref[1]: %" PRIX64 ", "
            "mnemonic: %X %X %X %X, fmt[0]: %d, fmt[1]: %d, flags: %X",
            header.m_top_ref[0], header.m_top_ref[1], header.m_mnemonic[0], header.m_mnemonic[1],
            header.m_mnemonic[2], header.m_mnemonic[3], header.m_file_format[0], header.m_file_format[1],
            header.m_flags);
    msg += buf;
    throw InvalidDatabase(msg, path);
}

// Note: This relies on proper mappings having been established by the caller
// for both the header and the streaming footer
ref_type SlabAlloc::validate_header(const char* data, size_t size, const std::string& path)
{
    auto header = reinterpret_cast<const Header*>(data);
    auto footer = reinterpret_cast<const StreamingFooter*>(data + size - sizeof(StreamingFooter));
    return validate_header(header, footer, size, path);
}

ref_type SlabAlloc::validate_header(const Header* header, const StreamingFooter* footer, size_t size,
                                    const std::string& path)
{
    // Verify that size is sane and 8-byte aligned
    if (REALM_UNLIKELY(size < sizeof(Header) || size % 8 != 0)) {
        std::string msg = "Realm file has bad size (" + util::to_string(size) + ")";
        throw InvalidDatabase(msg, path);
    }

    // First four bytes of info block is file format id
    if (REALM_UNLIKELY(!(char(header->m_mnemonic[0]) == 'T' && char(header->m_mnemonic[1]) == '-' &&
                         char(header->m_mnemonic[2]) == 'D' && char(header->m_mnemonic[3]) == 'B')))
        throw_header_exception("Invalid mnemonic", *header, path);

    // Last bit in info block indicates which top_ref block is valid
    int slot_selector = ((header->m_flags & SlabAlloc::flags_SelectBit) != 0 ? 1 : 0);

    // Top-ref must always point within buffer
    auto top_ref = header->m_top_ref[slot_selector];
    if (slot_selector == 0 && top_ref == 0xFFFFFFFFFFFFFFFFULL) {
        if (REALM_UNLIKELY(size < sizeof(Header) + sizeof(StreamingFooter))) {
            std::string msg = "Invalid streaming format size (" + util::to_string(size) + ")";
            throw InvalidDatabase(msg, path);
        }
        top_ref = footer->m_top_ref;
        if (REALM_UNLIKELY(footer->m_magic_cookie != footer_magic_cookie)) {
            std::string msg = "Invalid streaming format cookie (" + util::to_string(footer->m_magic_cookie) + ")";
            throw InvalidDatabase(msg, path);
        }
    }
    if (REALM_UNLIKELY(top_ref % 8 != 0)) {
        std::string msg = "Top ref not aligned (" + util::to_string(top_ref) + ")";
        throw_header_exception(msg, *header, path);
    }
    if (REALM_UNLIKELY(top_ref >= size)) {
        std::string msg = "Top ref outside file (size = " + util::to_string(size) + ")";
        throw_header_exception(msg, *header, path);
    }
    return ref_type(top_ref);
}


size_t SlabAlloc::get_total_size() const noexcept
{
    return m_slabs.empty() ? size_t(m_baseline.load(std::memory_order_relaxed)) : m_slabs.back().ref_end;
}


void SlabAlloc::reset_free_space_tracking()
{
    CriticalSection cs(changes);
    if (is_free_space_clean())
        return;

    // Free all scratch space (done after all data has
    // been commited to persistent space)
    m_free_read_only.clear();

    // release slabs.. keep the initial allocation if it's a minimal allocation,
    // otherwise release it as well. This saves map/unmap for small transactions.
    while (m_slabs.size() > 1 || (m_slabs.size() == 1 && m_slabs[0].size > minimal_alloc)) {
        auto& last_slab = m_slabs.back();
        auto& last_translation = m_ref_translation_ptr[m_translation_table_size - 1];
        REALM_ASSERT(last_translation.mapping_addr == last_slab.addr);
        --m_translation_table_size;
        m_slabs.pop_back();
    }
    rebuild_freelists_from_slab();
    m_free_space_state = free_space_Clean;
    m_commit_size = 0;
}

inline bool randomly_false_in_debug(bool x)
{
#ifdef REALM_DEBUG
    if (x)
        return (std::rand() & 1);
#endif
    return x;
}


/*
  Memory mapping

  To make ref->ptr translation fast while also avoiding to have to memory map the entire file
  contiguously (which is a problem for large files on 32-bit devices and most iOS devices), it is
  essential to map the file in even sized sections.

  These sections must be large enough to hold one or more of the largest arrays, which can be up
  to 16MB. You can only mmap file space which has been allocated to a file. If you mmap a range
  which extends beyond the last page of a file, the result is undefined, so we can't do that.
  We don't want to extend the file in increments as large as the chunk size.

  As the file grows, we grow the mapping by creating a new larger one, which replaces the
  old one in the mapping table. However, we must keep the old mapping open, because older
  read transactions will continue to use it. Hence, the replaced mappings are accumulated
  and only cleaned out once we know that no transaction can refer to them anymore.

  Interaction with encryption

  When encryption is enabled, the memory mapping is to temporary memory, not the file.
  The binding to the file is done by software. This allows us to "cheat" and allocate
  entire sections. With encryption, it doesn't matter if the mapped memory logically
  extends beyond the end of file, because it will not be accessed.

  Growing/Changing the mapping table.

  There are two mapping tables:

  * m_mappings: This is the "source of truth" about what the current mapping is.
    It is only accessed under lock.
  * m_fast_mapping: This is generated to match m_mappings, but is also accessed in a
    mostly lock-free fashion from the translate function. Because of the lock free operation this
    table can only be extended. Only selected members in each entry can be changed.
    See RefTranslation in alloc.hpp for more details.
    The fast mapping also maps the slab area used for allocations - as mappings are added,
    the slab area *moves*, corresponding to the movement of m_baseline. This movement does
    not need to trigger generation of a new m_fast_mapping table, because it is only relevant
    to memory allocation and release, which is already serialized (since write transactions are
    single threaded).

  When m_mappings is changed due to an extend operation changing a mapping, or when
  it has grown such that it cannot be reflected in m_fast_mapping, we use read-copy-update:

  * A new fast mapping table is created. The old one is not modified.
  * The old one is held in a waiting area until it is no longer relevant because no
    live transaction can refer to it any more.
 */
void SlabAlloc::update_reader_view(size_t file_size)
{
    std::lock_guard<std::mutex> lock(m_mapping_mutex);
    size_t old_baseline = m_baseline.load(std::memory_order_relaxed);
    if (file_size <= old_baseline) {
        return;
    }
    REALM_ASSERT_EX(file_size % 8 == 0, file_size, get_file_path_for_assertions()); // 8-byte alignment required
    REALM_ASSERT_EX(m_attach_mode == attach_SharedFile || m_attach_mode == attach_UnsharedFile, m_attach_mode,
                    get_file_path_for_assertions());
    REALM_ASSERT_DEBUG(is_free_space_clean());

    // Create the new mappings we needed to cover the new size. We don't mutate
    // any of the member variables until we've successfully created all of the
    // mappings so that we leave things in a consistent state if one of them
    // hits an allocation failure.
    bool replace_last_mapping = false;
    std::vector<MapEntry> new_mappings;
    const auto old_slab_base = align_size_to_section_boundary(old_baseline);
    size_t old_num_mappings = get_section_index(old_slab_base);
    REALM_ASSERT(m_mappings.size() == old_num_mappings);

    {
        // If the old slab base was greater than the old baseline then the final
        // mapping was a partial section and we need to replace it with a larger
        // mapping.
        if (old_baseline < old_slab_base) {
            // old_slab_base should be 0 if we had no mappings previously
            REALM_ASSERT(old_num_mappings > 0);
            replace_last_mapping = true;
            --old_num_mappings;
        }

        // Create new mappings covering from the end of the last complete
        // section to the end of the new file size.
        const auto new_slab_base = align_size_to_section_boundary(file_size);
        const size_t num_mappings = get_section_index(new_slab_base);
        new_mappings.reserve(num_mappings - old_num_mappings);
        for (size_t k = old_num_mappings; k < num_mappings; ++k) {
            const size_t section_start_offset = get_section_base(k);
            const size_t section_size = std::min<size_t>(1 << section_shift, file_size - section_start_offset);
            new_mappings.push_back(
                {util::File::Map<char>(m_file, section_start_offset, File::access_ReadOnly, section_size)});
        }
    }

    // Now that we've successfully created our mappings, update our member
    // variables (and assume that resizing a simple vector won't produce memory
    // allocation failures, unlike 64 MB mmaps).
    if (replace_last_mapping) {
        MapEntry& cur_entry = m_mappings.back();
        // We should not have a xover mapping here because that would mean
        // that there was already something mapped after the last section
        REALM_ASSERT(!cur_entry.xover_mapping.is_attached());
        // save the old mapping/keep it open
        m_old_mappings.push_back({m_youngest_live_version, std::move(cur_entry.primary_mapping)});
        m_mappings.pop_back();
        m_mapping_version++;
    }

    std::move(new_mappings.begin(), new_mappings.end(), std::back_inserter(m_mappings));
    m_baseline.store(file_size, std::memory_order_relaxed);

    const size_t ref_start = align_size_to_section_boundary(file_size);
    const size_t ref_displacement = ref_start - old_slab_base;
    if (ref_displacement > 0) {
        // Rebase slabs as m_baseline is now bigger than old_slab_base
        for (auto& e : m_slabs) {
            e.ref_end += ref_displacement;
        }
    }

    rebuild_freelists_from_slab();

    // Build the fast path mapping

    // The fast path mapping is an array which will is used from multiple threads
    // without locking - see translate().

    // Addition of a new mapping may require a completely new fast mapping table.
    //
    // Being used in a multithreaded scenario, the old mappings must be retained open,
    // until the realm version for which they were established has been closed/detached.
    //
    // This assumes that only write transactions call do_alloc() or do_free() or needs to
    // translate refs in the slab area, and that all these uses are serialized, whether
    // that is achieved by being single threaded, interlocked or run from a sequential
    // scheduling queue.
    //
    rebuild_translations(replace_last_mapping, old_num_mappings);
}

size_t SlabAlloc::get_allocated_size() const noexcept
{
    size_t sz = 0;
    for (const auto& s : m_slabs)
        sz += s.size;
    return sz;
}

void SlabAlloc::extend_fast_mapping_with_slab(char* address)
{
    ++m_translation_table_size;
    auto new_fast_mapping = std::make_unique<RefTranslation[]>(m_translation_table_size);
    for (size_t i = 0; i < m_translation_table_size - 1; ++i) {
        new_fast_mapping[i] = m_ref_translation_ptr[i];
    }
    m_old_translations.emplace_back(m_youngest_live_version, m_translation_table_size - m_slabs.size(),
                                    m_ref_translation_ptr.load());
    new_fast_mapping[m_translation_table_size - 1].mapping_addr = address;
    // Memory ranges with slab (working memory) can never have arrays that straddle a boundary,
    // so optimize by clamping the lowest possible xover offset to the end of the section.
    new_fast_mapping[m_translation_table_size - 1].lowest_possible_xover_offset = 1ULL << section_shift;
    m_ref_translation_ptr = new_fast_mapping.release();
}

void SlabAlloc::rebuild_translations(bool requires_new_translation, size_t old_num_sections)
{
    size_t free_space_size = m_slabs.size();
    auto num_mappings = m_mappings.size();
    if (m_translation_table_size < num_mappings + free_space_size) {
        requires_new_translation = true;
    }
    RefTranslation* new_translation_table = m_ref_translation_ptr;
    std::unique_ptr<RefTranslation[]> new_translation_table_owner;
    if (requires_new_translation) {
        // we need a new translation table, but must preserve old, as translations using it
        // may be in progress concurrently
        if (m_translation_table_size)
            m_old_translations.emplace_back(m_youngest_live_version, m_translation_table_size - free_space_size,
                                            m_ref_translation_ptr.load());
        m_translation_table_size = num_mappings + free_space_size;
        new_translation_table_owner = std::make_unique<RefTranslation[]>(m_translation_table_size);
        new_translation_table = new_translation_table_owner.get();
        old_num_sections = 0;
    }
    for (size_t i = old_num_sections; i < num_mappings; ++i) {
        new_translation_table[i].mapping_addr = m_mappings[i].primary_mapping.get_addr();
        REALM_ASSERT(new_translation_table[i].mapping_addr);
#if REALM_ENABLE_ENCRYPTION
        new_translation_table[i].encrypted_mapping = m_mappings[i].primary_mapping.get_encrypted_mapping();
#endif
        // We don't copy over data for the cross over mapping. If the mapping is needed,
        // copying will happen on demand (in get_or_add_xover_mapping).
        // Note: that may never be needed, because if the array that needed the original cross over
        // mapping is freed, any new array allocated at the same position will NOT need a cross
        // over mapping, but just use the primary mapping.
    }
    for (size_t k = 0; k < free_space_size; ++k) {
        char* base = m_slabs[k].addr;
        REALM_ASSERT(base);
        new_translation_table[num_mappings + k].mapping_addr = base;
    }

    // This will either be null or the same as new_translation_table, which is about to become owned by
    // m_ref_translation_ptr.
    (void)new_translation_table_owner.release();

    m_ref_translation_ptr = new_translation_table;
}

void SlabAlloc::get_or_add_xover_mapping(RefTranslation& txl, size_t index, size_t offset, size_t size)
{
    auto _page_size = page_size();
    std::lock_guard<std::mutex> lock(m_mapping_mutex);
    if (txl.xover_mapping_addr.load(std::memory_order_relaxed)) {
        // some other thread already added a mapping
        // it MUST have been for the exact same address:
        REALM_ASSERT(offset == txl.lowest_possible_xover_offset.load(std::memory_order_relaxed));
        return;
    }
    MapEntry* map_entry = &m_mappings[index];
    REALM_ASSERT(map_entry->primary_mapping.get_addr() == txl.mapping_addr);
    if (!map_entry->xover_mapping.is_attached()) {
        // Create a xover mapping
        auto file_offset = get_section_base(index) + offset;
        auto end_offset = file_offset + size;
        auto mapping_file_offset = file_offset & ~(_page_size - 1);
        auto minimal_mapping_size = end_offset - mapping_file_offset;
        util::File::Map<char> mapping(m_file, mapping_file_offset, File::access_ReadOnly, minimal_mapping_size);
        map_entry->xover_mapping = std::move(mapping);
    }
    txl.xover_mapping_base = offset & ~(_page_size - 1);
#if REALM_ENABLE_ENCRYPTION
    txl.xover_encrypted_mapping = map_entry->xover_mapping.get_encrypted_mapping();
#endif
    txl.xover_mapping_addr.store(map_entry->xover_mapping.get_addr(), std::memory_order_release);
}

void SlabAlloc::verify_old_translations(uint64_t youngest_live_version)
{
    // Verify that each old ref translation pointer still points to a valid
    // thing that we haven't released yet.
#if REALM_DEBUG
    std::unordered_set<const char*> mappings;
    for (auto& m : m_old_mappings) {
        REALM_ASSERT(m.mapping.is_attached());
        mappings.insert(m.mapping.get_addr());
    }
    for (auto& m : m_mappings) {
        REALM_ASSERT(m.primary_mapping.is_attached());
        mappings.insert(m.primary_mapping.get_addr());
        if (m.xover_mapping.is_attached())
            mappings.insert(m.xover_mapping.get_addr());
    }
    if (m_data)
        mappings.insert(m_data);
    for (auto& t : m_old_translations) {
        REALM_ASSERT_EX(youngest_live_version == 0 || t.replaced_at_version < youngest_live_version,
                        youngest_live_version, t.replaced_at_version);
        if (nonempty_attachment()) {
            for (size_t i = 0; i < t.translation_count; ++i)
                REALM_ASSERT(mappings.count(t.translations[i].mapping_addr));
        }
    }
#else
    static_cast<void>(youngest_live_version);
#endif
}


void SlabAlloc::purge_old_mappings(uint64_t oldest_live_version, uint64_t youngest_live_version)
{
    std::lock_guard<std::mutex> lock(m_mapping_mutex);
    verify_old_translations(youngest_live_version);

    auto pred = [=](auto& oldie) {
        return oldie.replaced_at_version < oldest_live_version;
    };
    m_old_mappings.erase(std::remove_if(m_old_mappings.begin(), m_old_mappings.end(), pred), m_old_mappings.end());
    m_old_translations.erase(std::remove_if(m_old_translations.begin(), m_old_translations.end(), pred),
                             m_old_translations.end());
    m_youngest_live_version = youngest_live_version;
    verify_old_translations(youngest_live_version);
}

void SlabAlloc::init_mapping_management(uint64_t currently_live_version)
{
    m_youngest_live_version = currently_live_version;
}

const SlabAlloc::Chunks& SlabAlloc::get_free_read_only() const
{
    if (REALM_COVER_NEVER(m_free_space_state == free_space_Invalid))
        throw InvalidFreeSpace();
    return m_free_read_only;
}


size_t SlabAlloc::find_section_in_range(size_t start_pos, size_t free_chunk_size, size_t request_size) const noexcept
{
    size_t end_of_block = start_pos + free_chunk_size;
    size_t alloc_pos = start_pos;
    while (alloc_pos + request_size <= end_of_block) {
        size_t next_section_boundary = get_upper_section_boundary(alloc_pos);
        if (alloc_pos + request_size <= next_section_boundary) {
            return alloc_pos;
        }
        alloc_pos = next_section_boundary;
    }
    return 0;
}


void SlabAlloc::resize_file(size_t new_file_size)
{
    REALM_ASSERT_EX(new_file_size == round_up_to_page_size(new_file_size), get_file_path_for_assertions());
    m_file.prealloc(new_file_size); // Throws
    // resizing is done based on the logical file size. It is ok for the file
    // to actually be bigger, but never smaller.
    REALM_ASSERT(new_file_size <= static_cast<size_t>(m_file.get_size()));

    bool disable_sync = get_disable_sync_to_disk() || m_cfg.disable_sync;
    if (!disable_sync)
        m_file.sync(); // Throws
}

#ifdef REALM_DEBUG
void SlabAlloc::reserve_disk_space(size_t size)
{
    if (size != round_up_to_page_size(size))
        size = round_up_to_page_size(size);
    m_file.prealloc(size); // Throws

    bool disable_sync = get_disable_sync_to_disk() || m_cfg.disable_sync;
    if (!disable_sync)
        m_file.sync(); // Throws
}
#endif

void SlabAlloc::verify() const
{
#ifdef REALM_DEBUG
    if (!m_slabs.empty()) {
        // Make sure that all free blocks are within a slab. This is done
        // implicitly by using for_all_free_entries()
        size_t first_possible_ref = m_baseline;
        size_t first_impossible_ref = align_size_to_section_boundary(m_slabs.back().ref_end);
        for_all_free_entries([&](size_t ref, size_t size) {
            REALM_ASSERT(ref >= first_possible_ref);
            REALM_ASSERT(ref + size <= first_impossible_ref);
            first_possible_ref = ref;
        });
    }
#endif
}

#ifdef REALM_DEBUG

bool SlabAlloc::is_all_free() const
{
    // verify that slabs contain only free space.
    // this is equivalent to each slab holding BetweenBlocks only at the ends.
    for (const auto& e : m_slabs) {
        auto first = reinterpret_cast<BetweenBlocks*>(e.addr);
        REALM_ASSERT(first->block_before_size == 0);
        auto last = reinterpret_cast<BetweenBlocks*>(e.addr + e.size) - 1;
        REALM_ASSERT(last->block_after_size == 0);
        if (first->block_after_size != last->block_before_size)
            return false;
        auto range = reinterpret_cast<char*>(last) - reinterpret_cast<char*>(first);
        range -= sizeof(BetweenBlocks);
        // the size of the free area must match the distance between the two BetweenBlocks:
        if (range != first->block_after_size)
            return false;
    }
    return true;
}


// LCOV_EXCL_START
void SlabAlloc::print() const
{
    /* TODO
     *

    size_t allocated_for_slabs = m_slabs.empty() ? 0 : m_slabs.back().ref_end - m_baseline;

    size_t free = 0;
    for (const auto& free_block : m_free_space) {
        free += free_block.size;
    }

    size_t allocated = allocated_for_slabs - free;
    std::cout << "Attached: " << (m_data ? size_t(m_baseline) : 0) << " Allocated: " << allocated << "\n";

    if (!m_slabs.empty()) {
        std::cout << "Slabs: ";
        ref_type first_ref = m_baseline;

        for (const auto& slab : m_slabs) {
            if (&slab != &m_slabs.front())
                std::cout << ", ";

            ref_type last_ref = slab.ref_end - 1;
            size_t size = slab.ref_end - first_ref;
            void* addr = slab.addr;
            std::cout << "(" << first_ref << "->" << last_ref << ", size=" << size << ", addr=" << addr << ")";
            first_ref = slab.ref_end;
        }
        std::cout << "\n";
    }

    if (!m_free_space.empty()) {
        std::cout << "FreeSpace: ";
        for (const auto& free_block : m_free_space) {
            if (&free_block != &m_free_space.front())
                std::cout << ", ";

            ref_type last_ref = free_block.ref + free_block.size - 1;
            std::cout << "(" << free_block.ref << "->" << last_ref << ", size=" << free_block.size << ")";
        }
        std::cout << "\n";
    }
    if (!m_free_read_only.empty()) {
        std::cout << "FreeSpace (ro): ";
        for (const auto& free_block : m_free_read_only) {
            if (&free_block != &m_free_read_only.front())
                std::cout << ", ";

            ref_type last_ref = free_block.ref + free_block.size - 1;
            std::cout << "(" << free_block.ref << "->" << last_ref << ", size=" << free_block.size << ")";
        }
        std::cout << "\n";
    }
    std::cout << std::flush;
    */
}
// LCOV_EXCL_STOP

#endif // REALM_DEBUG
