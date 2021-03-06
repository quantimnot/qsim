#ifndef __QCACHE_DIR_H
#define __QCACHE_DIR_H

#include <iostream>
#include <iomanip>

#include <vector>
#include <map>
#include <set>

#include <stdint.h>
#include <pthread.h>
#include <limits.h>

#include <stdlib.h>

#include "qcache.h"

namespace Qcache {
  // Directory for directory-based protocols.
  template <int L2LINESZ> class CoherenceDir {
  public:
    ~CoherenceDir() {
      if (!printResults) return;

      std::map<unsigned, unsigned> nSharers; // Map from #sharers to #lines
      unsigned maxSharers = 0;
      for (unsigned i = 0; i < DIR_BANKS; ++i) {
        typedef typename std::map<addr_t, Entry*>::iterator entries_it_t;
        for (entries_it_t it = banks[i].entries.begin();
             it != banks[i].entries.end(); ++it)
        {
          unsigned s(it->second->alltime.size());
          ++nSharers[s];
          if (s > maxSharers) maxSharers = s;
        }
      }

      std::cout << "Sharing histogram: ";
      for (unsigned i = 1; i <= maxSharers; ++i) {
	std::cout << nSharers[i];
        if (i != maxSharers) std::cout << ", ";
      }
      std::cout << '\n';
    }

    void lockAddr(addr_t addr, int id) {
      ASSERT(addr%(1<<L2LINESZ) == 0);
      if (banks[getBankIdx(addr)].getEntry(addr).lockHolder == id) return;
      spin_lock(&banks[getBankIdx(addr)].getEntry(addr).lock);
      #ifdef DEBUG
      pthread_mutex_lock(&errLock);
      std::cout << id << ": Lock " << std::hex << addr << '\n';
      pthread_mutex_unlock(&errLock);
      #endif
      banks[getBankIdx(addr)].getEntry(addr).lockHolder = id;
    }

    void unlockAddr(addr_t addr, int id) {
      ASSERT(addr%(1<<L2LINESZ) == 0);
      if (banks[getBankIdx(addr)].getEntry(addr).lockHolder != id) return;
      banks[getBankIdx(addr)].getEntry(addr).lockHolder = -1;
      spin_unlock(&banks[getBankIdx(addr)].getEntry(addr).lock);
      #ifdef DEBUG
      pthread_mutex_lock(&errLock);
      std::cout << id << ": Unlock " << std::hex << addr << '\n';
      pthread_mutex_unlock(&errLock);
      #endif
    }

    void addAddr(addr_t addr, int id) {
      ASSERT(addr%(1<<L2LINESZ) == 0);
      ASSERT(banks[getBankIdx(addr)].getEntry(addr).lockHolder == id);
      ASSERT(!hasId(addr, id));
#ifdef DEBUG
      pthread_mutex_lock(&errLock);
      std::cout << "0x" << std::hex << addr << ": add " << id << ';';
      printEntry(addr, std::cout, id);
      pthread_mutex_unlock(&errLock);
#endif
      banks[getBankIdx(addr)].getEntry(addr).present.insert(id);
      banks[getBankIdx(addr)].getEntry(addr).alltime.insert(id);
    }

    void remAddr(addr_t addr, int id) {
      ASSERT(addr%(1<<L2LINESZ) == 0);
#ifdef DEBUG
      pthread_mutex_lock(&errLock);
      std::cout << "0x" << std::hex << addr << ": rem " << id << ';';
      printEntry(addr, std::cout, id);
      pthread_mutex_unlock(&errLock);
#endif

      if (!hasId(addr, id)) {
        // This is an error condition, but may happen e.g. because of icache.
        // It represents a possible lapse in correctness, and benchmarks that
        // cause this condition to occur frequently will not generate reliable
        // results.
#if 0
	std::cout << "WARNING: attempt to evict line that was already absent "
                     "from directory.\n";
#endif
        return;
      }

#ifdef ENABLE_ASSERTIONS
      if (banks[getBankIdx(addr)].getEntry(addr).lockHolder != id) {
	std::cerr << "Error: Tried to remove on cache " << id 
                  << " while lock held by " 
                  << banks[getBankIdx(addr)].getEntry(addr).lockHolder << ".\n";
        ASSERT(false);
      }
#endif

      banks[getBankIdx(addr)].getEntry(addr).present.erase(id);
    }

    bool hasId(addr_t addr, int id) {
      ASSERT(addr%(1<<L2LINESZ) == 0);
      ASSERT(banks[getBankIdx(addr)].getEntry(addr).lockHolder == id);
      return banks[getBankIdx(addr)].getEntry(addr).present.find(id) !=
               banks[getBankIdx(addr)].getEntry(addr).present.end();
    }

    std::set<int>::iterator idsBegin(addr_t addr, int id) {
      ASSERT(addr%(1<<L2LINESZ) == 0);
      ASSERT(banks[getBankIdx(addr)].getEntry(addr).lockHolder == id);
      return banks[getBankIdx(addr)].getEntry(addr).present.begin();
    }

    std::set<int>::iterator idsEnd(addr_t addr, int id) {
      ASSERT(addr%(1<<L2LINESZ) == 0);
      ASSERT(banks[getBankIdx(addr)].getEntry(addr).lockHolder == id);
      return banks[getBankIdx(addr)].getEntry(addr).present.end();
    }

    std::set<int>::iterator clearIds(addr_t addr, int remaining) {
      ASSERT(addr%(1<<L2LINESZ) == 0);
      ASSERT(banks[getBankIdx(addr)].getEntry(addr).lockHolder == remaining);
#ifdef DEBUG
      pthread_mutex_lock(&errLock);
      std::cout << "0x" << std::hex << addr << ": clear " << remaining << ';';
      printEntry(addr, std::cout, remaining);
      pthread_mutex_unlock(&errLock);
#endif
      std::set<int> &p(banks[getBankIdx(addr)].getEntry(addr).present);
      p.clear();
      p.insert(remaining);
    }

    void printEntry(addr_t addr, std::ostream &os, int id) {
      for (std::set<int>::iterator it = idsBegin(addr, id);
           it != idsEnd(addr, id); ++it)
      {
        os << ' ' << *it;
      }
      os << '\n';
    }

  private:
    struct Entry {
      Entry(): present() { spinlock_init(&lock); }
      spinlock_t lock;
      int lockHolder;
      std::set<int> present;
      std::set<int> alltime;
    };

    struct Bank {
      Bank(): entries() { spinlock_init(&lock); }

      // We want to use a map<addr_t, Entry> since the address space will tend
      // to be fragmented, but we want it to be thread safe. This code ensures
      // that.
      Entry &getEntry(addr_t addr) {
        Entry *rval;
        spin_lock(&lock);
        rval = entries[addr];
        if (!rval) entries[addr] = rval = new Entry();
        spin_unlock(&lock);

        return *rval;
      }

      spinlock_t lock;
      std::map<addr_t, Entry*> entries;
    };

    size_t getBankIdx(addr_t addr) {
      return (addr>>L2LINESZ)%DIR_BANKS;
    }

    Bank banks[DIR_BANKS];
  };
};

#endif
