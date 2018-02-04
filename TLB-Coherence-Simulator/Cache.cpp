//
//  Cache.cpp
//  TLB-Coherence-Simulator
//
//  Created by Yashwant Marathe on 12/6/17.
//  Copyright © 2017 Yashwant Marathe. All rights reserved.
//

#include "Cache.hpp"
#include "CacheSys.hpp"
#include <assert.h>
#include <vector>
#include <iomanip>
#include "utils.hpp"
#include "Core.hpp"

uint64_t Cache::get_line_offset(const uint64_t addr)
{
    return (addr & ((1 << m_num_line_offset_bits) - 1));
}

uint64_t Cache::get_index(const uint64_t addr)
{
    return (addr >> m_num_line_offset_bits) & (((1 << m_num_index_bits) - 1));
}

uint64_t Cache::get_tag(const uint64_t addr)
{
    return ((addr >> m_num_line_offset_bits) >> m_num_index_bits);
}

bool Cache::is_found(const std::vector<CacheLine>& set, const uint64_t tag, bool is_translation, uint64_t tid, unsigned int &hit_pos)
{
    auto it = std::find_if(set.begin(), set.end(), [tag, is_translation, tid, this](const CacheLine &l)
                           {
                               return ((l.tag == tag) && (l.valid) && (l.is_translation == is_translation) && (l.tid == tid));
                           });
    
    hit_pos = static_cast<unsigned int>(it - set.begin());
    return (it != set.end());
}

bool Cache::is_hit(const std::vector<CacheLine> &set, const uint64_t tag, bool is_translation, uint64_t tid, unsigned int &hit_pos)
{
    return is_found(set, tag, is_translation, tid, hit_pos) & !set[hit_pos].lock;
}

void Cache::invalidate(const uint64_t addr, uint64_t tid, bool is_translation)
{
    unsigned int hit_pos;
    uint64_t tag = get_tag(addr);
    uint64_t index = get_index(addr);
    std::vector<CacheLine>& set = m_tagStore[index];
    
    //If we find line in the cache, invalidate the line
    
    if(is_found(set, tag, is_translation, tid, hit_pos))
    {
        set[hit_pos].valid = false;
    }
    
    //Go all the way up to highest cache
    //There might be more than one higher cache (example L3)
    try
    {
        for(int i = 0; i < m_higher_caches.size(); i++)
        {
            auto higher_cache = m_higher_caches[i].lock();
            if(higher_cache != nullptr)
            {
                higher_cache->invalidate(addr, tid, is_translation);
            }
        }
    }
    catch(std::bad_weak_ptr &e)
    {
        std::cout << "Cache " << m_cache_level << ":" << e.what() << std::endl;
    }
}

void Cache::evict(uint64_t set_num, const CacheLine &line)
{
    //Send back invalidate
    
    uint64_t evict_addr = ((line.tag << m_num_line_offset_bits) << m_num_index_bits) | (set_num << m_num_line_offset_bits);
    
    if(m_inclusive)
    {
        try
        {
            //No harm in blindly invalidating, since is_found checks is_translation.
            for(int i = 0; i < m_higher_caches.size(); i++)
            {
                auto higher_cache = m_higher_caches[i].lock();
                if(higher_cache != nullptr)
                {
                    higher_cache->invalidate(evict_addr, line.tid, line.is_translation);
                }
            }
        }
        catch(std::bad_weak_ptr &e)
        {
            std::cout << "Cache " << m_cache_level << "doesn't have a valid higher cache" << std::endl;
        }
    }
    
    //Send writeback if dirty
    //If not, due to inclusiveness, lower caches still have data
    //So do runtime check to ensure hit (and inclusiveness)
    std::shared_ptr<Cache> lower_cache = find_lower_cache_in_core(evict_addr, line.is_translation, line.is_large);
    
    if(line.dirty)
    {
        if(lower_cache != nullptr)
        {
            RequestStatus val = lower_cache->lookupAndFillCache(evict_addr, line.is_translation ? TRANSLATION_WRITEBACK : DATA_WRITEBACK, line.tid, line.is_large);
            line.m_coherence_prot->forceCoherenceState(INVALID);
            
            if(m_inclusive)
            {
                assert((val == REQUEST_HIT) || (val == MSHR_HIT_AND_LOCKED));
            }
        }
        else
        {
            //Writeback to memory
        }
    }
    else
    {
        if(lower_cache != nullptr && m_inclusive)
        {
            unsigned int hit_pos;
            uint64_t index = lower_cache->get_index(evict_addr);
            uint64_t tag = lower_cache->get_index(evict_addr);
            std::vector<CacheLine> set = lower_cache->m_tagStore[index];
            assert(lower_cache->is_found(set, tag, line.is_translation, line.tid, hit_pos));
        }
    }
}

RequestStatus Cache::lookupAndFillCache(uint64_t addr, kind txn_kind, uint64_t tid, bool is_large, unsigned int curr_latency)
{
    unsigned int hit_pos;
    uint64_t tag = get_tag(addr);
    uint64_t index = get_index(addr);
    std::vector<CacheLine>& set = m_tagStore[index];
    uint64_t coh_addr;
    bool coh_is_translation;
    uint64_t coh_tid;
    bool coh_is_large;
    uint64_t cur_addr;
    
    bool is_translation = (txn_kind == TRANSLATION_WRITE) | (txn_kind == TRANSLATION_WRITEBACK) | (txn_kind == TRANSLATION_READ);
    
    if(is_hit(set, tag, is_translation, tid, hit_pos))
    {
        CacheLine &line = set[hit_pos];
        
        cur_addr = ((line.tag << m_num_line_offset_bits) << m_num_index_bits) | (index << m_num_line_offset_bits);
        
        //Is line dirty now?
        line.dirty = line.dirty || (txn_kind == DATA_WRITE) || (txn_kind == TRANSLATION_WRITE) || (txn_kind == DATA_WRITEBACK) || (txn_kind == TRANSLATION_WRITEBACK);
        
        assert(!((txn_kind == TRANSLATION_WRITE) | (txn_kind == TRANSLATION_WRITEBACK) | (txn_kind == TRANSLATION_READ)) ^(line.is_translation));
        
        //Update replacement state
        if(txn_kind != TRANSLATION_WRITEBACK && txn_kind != DATA_WRITEBACK)
        {
            m_repl->updateReplState(index, hit_pos);
        }
        
        m_callback = std::bind(&Cache::release_lock, this, std::placeholders::_1);
        std::unique_ptr<Request> r = std::make_unique<Request>(Request(addr, txn_kind, m_callback, tid, is_large, m_core_id));
        m_cache_sys->m_hit_list.insert(std::make_pair(m_cache_sys->m_clk + curr_latency, std::move(r)));
        
        //Coherence handling
        CoherenceAction coh_action = line.m_coherence_prot->setNextCoherenceState(txn_kind);
        
        //If we need to do writeback, we need to do it for addr already in cache
        //If we need to broadcast, we need to do it for addr in lookupAndFillCache call
        coh_addr = (coh_action == MEMORY_DATA_WRITEBACK || coh_action == MEMORY_TRANSLATION_WRITEBACK) ? cur_addr : addr;
        coh_is_translation = (coh_action == MEMORY_DATA_WRITEBACK || coh_action == MEMORY_TRANSLATION_WRITEBACK) ? line.is_translation : is_translation;
        coh_tid = (coh_action == MEMORY_DATA_WRITEBACK || coh_action == MEMORY_TRANSLATION_WRITEBACK) ? line.tid : tid;
        coh_is_large = (coh_action == MEMORY_DATA_WRITEBACK || coh_action == MEMORY_TRANSLATION_WRITEBACK) ? line.is_large : is_large;
        
        handle_coherence_action(coh_action, coh_addr, coh_tid, coh_is_large, curr_latency, coh_is_translation, true);
        
        return REQUEST_HIT;
    }
    
    auto it_not_valid = std::find_if(set.begin(), set.end(), [](const CacheLine &l) { return !l.valid; });
    bool needs_eviction = (it_not_valid == set.end()) && (!is_found(set, tag, is_translation, tid, hit_pos));
  
    //Where are we inserting the line?
    //If line is allocated in cache set, choose that position
    //If line is not allocated, choose victim
    unsigned int insert_pos = is_found(set, tag, is_translation, tid, hit_pos)? hit_pos : m_repl->getVictim(set, index);
    
    CacheLine &line = set[insert_pos];
    
    auto mshr_iter = m_mshr_entries.find(addr);
    
    cur_addr = ((line.tag << m_num_line_offset_bits) << m_num_index_bits) | (index << m_num_line_offset_bits);
    
    //Blocking for TLB access.
    unsigned int mshr_size = (m_cache_type != TRANSLATION_ONLY) ? 16 : 1;
    
    //Only if line is valid, we consider it to be an MSHR hit.
    if(mshr_iter != m_mshr_entries.end() && line.valid)
    {
        //MSHR hit
        if(((txn_kind == TRANSLATION_WRITE) || (txn_kind == DATA_WRITE) || (txn_kind == TRANSLATION_WRITEBACK) || (txn_kind == DATA_WRITEBACK)) && \
                (get_tag(addr) == mshr_iter->second->m_line->tag) && \
                (is_translation == mshr_iter->second->m_line->is_translation))
        {
            mshr_iter->second->m_line->dirty = true;
        }
        
        if(get_tag(addr) == mshr_iter->second->m_line->tag && \
            (is_translation == mshr_iter->second->m_line->is_translation))
        {
            CoherenceAction coh_action = mshr_iter->second->m_line->m_coherence_prot->setNextCoherenceState(txn_kind);
            
            //If we need to do writeback, we need to do it for addr already in cache
            //If we need to broadcast, we need to do it for addr in lookupAndFillCache call
        
            uint64_t coh_addr = (coh_action == MEMORY_DATA_WRITEBACK || coh_action == MEMORY_TRANSLATION_WRITEBACK) ? cur_addr : addr;
            
            coh_addr = (coh_action == MEMORY_DATA_WRITEBACK || coh_action == MEMORY_TRANSLATION_WRITEBACK) ? cur_addr : addr;
            coh_is_translation = (coh_action == MEMORY_DATA_WRITEBACK || coh_action == MEMORY_TRANSLATION_WRITEBACK) ? line.is_translation : is_translation;
            coh_tid = (coh_action == MEMORY_DATA_WRITEBACK || coh_action == MEMORY_TRANSLATION_WRITEBACK) ? line.tid : tid;
            coh_is_large = (coh_action == MEMORY_DATA_WRITEBACK || coh_action == MEMORY_TRANSLATION_WRITEBACK) ? line.is_large : is_large;
            
            handle_coherence_action(coh_action, coh_addr, coh_tid, coh_is_large, curr_latency, coh_is_translation, true);
        }
        
        //Update replacement state
        if(txn_kind != TRANSLATION_WRITEBACK && txn_kind != DATA_WRITEBACK)
        {
            m_repl->updateReplState(index, insert_pos);
        }
        
        if(txn_kind == TRANSLATION_WRITEBACK || txn_kind == DATA_WRITEBACK)
        {
            assert(mshr_iter->second->m_line->lock);
            return MSHR_HIT_AND_LOCKED;
        }
        else
        {
            return MSHR_HIT;
        }
    }
    else if(m_mshr_entries.size() < mshr_size)
    {
        //MSHR miss, add entry
        line.valid = true;
        line.lock = true;
        line.tag = tag;
        line.is_translation = (txn_kind == TRANSLATION_READ) || (txn_kind == TRANSLATION_WRITE) || (txn_kind == TRANSLATION_WRITEBACK);
        line.is_large = is_large;
        line.tid = tid;
        line.dirty = (txn_kind == TRANSLATION_WRITE) || (txn_kind == DATA_WRITE) || (txn_kind == TRANSLATION_WRITEBACK) || (txn_kind == DATA_WRITEBACK);
        MSHREntry *mshr_entry = new MSHREntry(txn_kind, &line);
        m_mshr_entries.insert(std::make_pair(addr, mshr_entry));
        
        //Evict the victim line if not locked and update replacement state
        if(needs_eviction)
        {
            evict(index, line);
        }
        
        if(txn_kind != TRANSLATION_WRITEBACK && txn_kind != DATA_WRITEBACK)
        {
            m_repl->updateReplState(index, insert_pos);
        }
    }
    else
    {
        //MSHR full
        return REQUEST_RETRY;
    }
    
    //We are in upper levels of TLB/cache and we aren't doing writeback.
    //Go to lower caches and do lookup.
    if(!m_cache_sys->is_last_level(m_cache_level) && ((txn_kind != DATA_WRITEBACK) || (txn_kind != TRANSLATION_WRITEBACK)))
    {
        std::shared_ptr<Cache> lower_cache = find_lower_cache_in_core(addr, is_translation, is_large);
        if(lower_cache != nullptr)
        {
            CacheType lower_cache_type = lower_cache->get_cache_type();
            bool is_tr_to_dat_boundary = (m_cache_type == TRANSLATION_ONLY) && (lower_cache_type == DATA_AND_TRANSLATION);
            uint64_t access_addr = (is_tr_to_dat_boundary) ? m_core->getL3TLBAddr(addr, tid, is_large): addr;
            lower_cache->lookupAndFillCache(access_addr, txn_kind, tid, is_large, curr_latency + m_latency_cycles);
        }
    }
    //We are in last level of cache hier and handling a data entry
    //We are in last level of TLB hier and handling a TLB entry
    //Go to memory.
    else if((m_cache_sys->is_last_level(m_cache_level) && !is_translation && !m_cache_sys->get_is_translation_hier()) || \
            (m_cache_sys->is_last_level(m_cache_level) && is_translation && (m_cache_sys->get_is_translation_hier())))
    {
        //TODO:: YMARATHE. Move std::bind elsewhere, performance hit
        m_callback = std::bind(&Cache::release_lock, this, std::placeholders::_1);
        std::unique_ptr<Request> r = std::make_unique<Request>(Request(addr, txn_kind, m_callback, tid, is_large, m_core_id));
        m_cache_sys->m_wait_list.insert(std::make_pair(m_cache_sys->m_clk + curr_latency + m_cache_sys->m_memory_latency, std::move(r)));
        
        std::cout << "Sending request to memory with latency = " << (curr_latency + m_cache_sys->m_memory_latency) << std::endl;
    }
    //We are in last level of cache hier and translation entry.
    //Go to L3 TLB.
    else
    {
        std::shared_ptr<Cache> lower_cache = find_lower_cache_in_core(addr, is_translation, is_large);
        if(lower_cache != nullptr)
        {
            lower_cache->lookupAndFillCache(addr, txn_kind, tid, is_large, curr_latency + m_latency_cycles);
        }
    }
 
    CoherenceAction coh_action = line.m_coherence_prot->setNextCoherenceState(txn_kind);
    
    coh_addr = (coh_action == MEMORY_DATA_WRITEBACK || coh_action == MEMORY_TRANSLATION_WRITEBACK) ? cur_addr : addr;
    coh_is_translation = (coh_action == MEMORY_DATA_WRITEBACK || coh_action == MEMORY_TRANSLATION_WRITEBACK) ? line.is_translation : is_translation;
    coh_tid = (coh_action == MEMORY_DATA_WRITEBACK || coh_action == MEMORY_TRANSLATION_WRITEBACK) ? line.tid : tid;
    coh_is_large = (coh_action == MEMORY_DATA_WRITEBACK || coh_action == MEMORY_TRANSLATION_WRITEBACK) ? line.is_large : is_large;
    
    handle_coherence_action(coh_action, coh_addr, coh_tid, coh_is_large, curr_latency, coh_is_translation, true);
    
    return REQUEST_MISS;
}

void Cache::add_lower_cache(const std::weak_ptr<Cache>& c)
{
    m_lower_cache = c;
}

void Cache::add_higher_cache(const std::weak_ptr<Cache>& c)
{
    m_higher_caches.push_back(c);
}

void Cache::set_level(unsigned int level)
{
    m_cache_level = level;
}

unsigned int Cache::get_level()
{
    return m_cache_level;
}

void Cache::printContents()
{
    for(auto &set: m_tagStore)
    {
        for(auto &line : set)
        {
            std::cout << *(line.m_coherence_prot) << line;
        }
        std::cout << std::endl;
    }
}

void Cache::set_cache_sys(CacheSys *cache_sys)
{
    m_cache_sys = cache_sys;
}

void Cache::release_lock(std::unique_ptr<Request>& r)
{
    auto it = m_mshr_entries.find(r->m_addr);
    
    //std::cout << "Request at level::" << m_cache_level << ", Addr::" << std::hex << r->m_addr << std::endl;
    
    if(it != m_mshr_entries.end())
    {
        //Handle corner case where a line is evicted when it is still in the 'lock' state
        //In this case, tag of the line would have changed, and hence we don't want to change the lock state.
        
        if(get_tag(r->m_addr) == it->second->m_line->tag)
        {
            //std::cout << "Found in MSHR::" << m_cache_level << ", changing lock bit, Addr::" << r->m_addr << std::endl;
            it->second->m_line->lock = false;
        }
        
        delete(it->second);
        
        m_mshr_entries.erase(it);
        
        //Ensure erasure in the MSHR
        assert(m_mshr_entries.find(r->m_addr) == m_mshr_entries.end());
    }
    
    //We are in L1
    if(m_cache_level == 1 && m_cache_type == DATA_ONLY)
    {
        m_core->m_rob.mem_mark_done(r->m_addr, r->m_type);
    }
    
    propagate_release_lock(r);
}

unsigned int Cache::get_latency_cycles()
{
    return m_latency_cycles;
}

void Cache::handle_coherence_action(CoherenceAction coh_action, uint64_t addr, uint64_t tid, bool is_large, unsigned int curr_latency, bool is_translation, bool same_cache_sys)
{
    if(coh_action == MEMORY_TRANSLATION_WRITEBACK || coh_action == MEMORY_DATA_WRITEBACK)
    {
        //Evict to lower cache if we need to do a writeback.
        std::shared_ptr<Cache> lower_cache = find_lower_cache_in_core(addr, is_translation, is_large);
        if(lower_cache != nullptr)
        {
            kind coh_txn_kind = txnKindForCohAction(coh_action);
            lower_cache->lookupAndFillCache(addr, coh_txn_kind, tid, is_large, curr_latency + m_latency_cycles);
        }
    }
    
    else if(coh_action == BROADCAST_DATA_READ || coh_action == BROADCAST_DATA_WRITE || \
            coh_action == BROADCAST_TRANSLATION_READ || coh_action == BROADCAST_TRANSLATION_WRITE
            )
    {
        //Update caches in all other cache hierarchies if call is from the same hierarchy
        //Pass addr and coherence action enforced by this cache
        if(same_cache_sys && !m_cache_sys->is_last_level(m_cache_level))
        {
            for(int i = 0; i < m_cache_sys->m_other_cache_sys.size(); i++)
            {
                m_callback = std::bind(&Cache::release_lock, this, std::placeholders::_1);
                //Inserting dummy TRANSLATION_WRITE and DATA_WRITE requests to decode whether coherence action was actually triggered by a translation request.
                std::unique_ptr<Request> r = std::make_unique<Request>(Request(addr, is_translation ? TRANSLATION_WRITE : DATA_WRITE, m_callback, tid, is_large, m_core_id));
                m_cache_sys->m_other_cache_sys[i]->m_coh_act_list.insert(std::make_pair(std::move(r), coh_action));
            }
        }
        else if(!same_cache_sys)
        {
            //If call came from different cache hierarchy, handle it!
            //Invalidate if we see BROADCAST_*_WRITE
            unsigned int hit_pos;
            uint64_t tag = get_tag(addr);
            uint64_t index = get_index(addr);
            std::vector<CacheLine>& set = m_tagStore[index];
            bool is_translation = (coh_action == BROADCAST_TRANSLATION_WRITE) || (coh_action == BROADCAST_TRANSLATION_READ);
            if(is_found(set, tag, tid, is_translation, hit_pos))
            {
                CacheLine &line = set[hit_pos];
                kind coh_txn_kind = txnKindForCohAction(coh_action);
                line.m_coherence_prot->setNextCoherenceState(coh_txn_kind);
                if(coh_txn_kind == DIRECTORY_DATA_WRITE || coh_txn_kind == DIRECTORY_TRANSLATION_WRITE)
                {
                    line.valid = false;
                    assert(line.m_coherence_prot->getCoherenceState() == INVALID);
                }
            }
        }
    }
}

void Cache::set_cache_type(CacheType cache_type)
{
    m_cache_type = cache_type;
}

CacheType Cache::get_cache_type()
{
    return m_cache_type;
}

void Cache::set_core(std::shared_ptr<Core>& coreptr)
{
    m_core = coreptr;
}

std::shared_ptr<Cache> Cache::find_lower_cache_in_core(uint64_t addr, bool is_translation, bool is_large)
{
    std::shared_ptr<Cache> lower_cache;
    
    //Check if lower cache is statically determined
    try
    {
        lower_cache = m_lower_cache.lock();
    }
    catch(std::bad_weak_ptr &e)
    {
        std::cout << "Cache " << m_cache_level << "doesn't have a valid lower cache" << std::endl;
    }
    
    //Determine lower_cache dynamically based on the type of transaction
    if(lower_cache == nullptr)
    {
        lower_cache = m_core->get_lower_cache(addr, is_translation, is_large, m_cache_level, m_cache_type);
    }
    
    return lower_cache;
}

void Cache::propagate_release_lock(std::unique_ptr<Request> &r)
{
    try
    {
        for(int i = 0; i < m_higher_caches.size(); i++)
        {
            auto higher_cache = m_higher_caches[i].lock();
            if(higher_cache != nullptr)
            {
                if(((r->is_translation_request() && higher_cache->get_cache_type() != DATA_ONLY) ||
                    (!r->is_translation_request() && higher_cache->get_cache_type() != TRANSLATION_ONLY)) && \
                   ((r->m_core_id == higher_cache->get_core_id() && m_cache_sys->is_last_level(m_cache_level) && !m_cache_sys->get_is_translation_hier()) || !m_cache_sys->is_last_level(m_cache_level) || (m_cache_sys->is_last_level(m_cache_level) && m_cache_sys->get_is_translation_hier())))
                {
                    
                    bool is_dat_to_tr_boundary = (m_cache_type == DATA_AND_TRANSLATION) && (higher_cache->get_cache_type() == TRANSLATION_ONLY);
                    bool in_translation_hier = (m_cache_type == TRANSLATION_ONLY) && (higher_cache->get_cache_type() == TRANSLATION_ONLY);
                    bool propagate_access = true;
                    bool is_higher_cache_small_tlb = !higher_cache->get_is_large_page_tlb();
                    uint64_t access_addr = (is_dat_to_tr_boundary) ? m_core->retrieveAddr(r->m_addr, r->m_tid, r->m_is_large, is_higher_cache_small_tlb, &propagate_access) : r->m_addr;
                    propagate_access = (propagate_access) && ((in_translation_hier && (r->m_is_large == !is_higher_cache_small_tlb)) || !in_translation_hier);
                    r->m_addr = (propagate_access) ? access_addr : r->m_addr;
                    if(propagate_access)
                    {
                        higher_cache->release_lock(r);
                    }
                }
            }
        }
    }
    catch(std::bad_weak_ptr &e)
    {
        std::cout << e.what() << std::endl;
    }
}

bool Cache::get_is_large_page_tlb()
{
    return m_is_large_page_tlb;
}

void Cache::set_core_id(unsigned int core_id)
{
    m_core_id = core_id;
}

unsigned int Cache::get_core_id()
{
    return m_core_id;
}
