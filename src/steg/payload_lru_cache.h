#ifndef _PAYLOAD_LRU_CACHE_H
#define _PAYLOAD_LRU_CACHE_H
 
#include <cassert> 
#include <list> 
#include <algorithm>

#include <util.h> 
// Class providing fixed-size (by number of records) 
// LRU-replacement cache of a function with signature 
// V f(K). 
// MAP should be one of std::map or std::unordered_map. 
// Variadic template args used to deal with the 
// different type argument signatures of those 
// containers; the default comparator/hash/allocator 
// will be used. 
template < 
  typename K, 
  typename V,
  typename RETRIEVER,
  template<typename...> class MAP 
  > class PayloadLRUCache
{ 
public: 
  typedef K key_type; 
  typedef V value_type; 
 
  // Key access history, most recent at back 
  typedef std::list<key_type> key_tracker_type; 
 
  // Key to value and key history iterator 
  typedef MAP< 
    key_type, 
    std::pair< 
      value_type, 
      typename key_tracker_type::iterator 
      > 
  > key_to_value_type; 

  // Constuctor specifies the cached function and 
  // the maximum number of records to be stored 
  PayloadLRUCache( 
  RETRIEVER* retriever_object,
  value_type (RETRIEVER::*f)(const key_type&),
    size_t c 
  ) 
    :retriever(retriever_object),
     _fn(f),
    _capacity(c) 
  
  { 
    assert(_capacity!=0); 
  } 
 
  // Obtain reference to the value of the cached function for k
  value_type& operator()(const key_type& k) { 
 
    // Attempt to find existing record 
    typename key_to_value_type::iterator it 
      =_key_to_value.find(k); 
 
    if (it==_key_to_value.end()) { 
      log_debug("payload cache MISS");
      
      // We don't have it: 
 
      // Evaluate function and create new record 
      const value_type v = (retriever->*_fn)(k);
      insert(k,v); 
 
      // now we should be able to find this key
      // in the cache.
      it = _key_to_value.find(k);
 
    } else { 
      // We do have it: 
      log_debug("payload cache HIT");

    }
  
    assert(it != _key_to_value.end());
    // Update access record by moving 
    // accessed key to back of list 
    _key_tracker.splice( 
                        _key_tracker.end(), 
                        _key_tracker, 
                        (*it).second.second 
                         ); 

    // Return the retrieved value 
    return (*it).second.first; 

  } 
 
  // Obtain the cached keys, most recently used element 
  // at head, least recently used at tail. 
  // This method is provided purely to support testing. 
  template <typename IT> void get_keys(IT dst) const { 
    typename key_tracker_type::const_reverse_iterator src 
      =_key_tracker.rbegin(); 
    while (src!=_key_tracker.rend()) { 
      *dst++ = *src++; 
    } 
  }

  /**
     drops the entry in the cache associtaed to the given key

     @param key the key pointing to the entry to erase

     @return true in case of success, false in case of failure (key not fount)
  */
  bool drop(const key_type& k) {
    typename key_to_value_type::iterator it 
      =_key_to_value.find(k);

    //if not found nothing to do
    if (it==_key_to_value.end())
      return false;

    //if found we need to drop the key from the tracker also
    auto dropped_key = std::find(_key_tracker.begin(),_key_tracker.end(), k);
    assert(dropped_key != _key_tracker.end()); //otherwise internal error

    _key_tracker.erase(dropped_key);
    _key_to_value.erase(it);

    return true;

  }
 
private: 
 
  // Record a fresh key-value pair in the cache 
  void insert(const key_type& k, const value_type& v) { 
 
    // Method is only called on cache misses 
    assert(_key_to_value.find(k)==_key_to_value.end()); 
 
    log_debug("payload cache is holding %zu elements of %zu capacity", _key_to_value.size(),_capacity);
    // Make space if necessary 
    if (_key_to_value.size()==_capacity) 
      evict(); 
 
    // Record k as most-recently-used key 
    typename key_tracker_type::iterator it 
      =_key_tracker.insert(_key_tracker.end(),k); 
 
    // Create the key-value entry, 
    // linked to the usage record. 
    _key_to_value.insert( 
      std::make_pair( 
        k, 
        std::make_pair(v,it) 
      ) 
    ); 
    // No need to check return, 
    // given previous assert. 
  } 
 
  // Purge the least-recently-used element in the cache 
  void evict() { 
 
    // Assert method is never called when cache is empty 
    assert(!_key_tracker.empty()); 
 
    // Identify least recently used key 
    const typename key_to_value_type::iterator it 
      =_key_to_value.find(_key_tracker.front()); 
    assert(it!=_key_to_value.end()); 
 
    // Erase both elements to completely purge record 
    _key_to_value.erase(it); 
    _key_tracker.pop_front(); 
  } 

  //pointer to the object that owns the _fn functions
  RETRIEVER* retriever;
  // The function to be cached 
  value_type (RETRIEVER::*_fn)(const key_type&); 
 
  // Maximum number of key-value pairs to be retained 
  const size_t _capacity; 
 
  // Key access history 
  key_tracker_type _key_tracker; 
 
  // Key-to-value lookup 
  key_to_value_type _key_to_value; 
}; 
 
#endif

