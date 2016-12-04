#ifndef CACHE_SET_RANDOM_H
#define CACHE_SET_RANDOM_H

#include "cache_set.h"

class CacheSetRandom : public CacheSet
{
   public:
      CacheSetRandom(CacheBase::cache_t cache_type,
            UInt32 associativity, UInt32 blocksize);
      ~CacheSetRandom();

      UInt32 getReplacementIndex(CacheCntlr *cntlr, int avoid_index = -1, int avoid_index2 = -1);
      void updateReplacementIndex(UInt32 accessed_index);

   private:
      Random m_rand;
};

#endif /* CACHE_SET_RANDOM_H */
