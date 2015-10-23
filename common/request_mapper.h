#ifndef _REQUEST_MAPPER_H_INCLUDED_
#define _REQUEST_MAPPER_H_INCLUDED_

#include <map>
#include <memory>

#if 0
template <typename T, typename U>
class Map 
{
    public:
    void put(const T& key, const T& value)
    {
        std::unique_lock<std::mutex> mlock(mutex_);
        rmap[key] = value;
        mlock.unlock();
    }
    U get(const T& key) 
    {
        std::unique_lock<std::mutex> mlock(mutex_);
        std::map<T, U>::iterator it = rmap.find(key);
        U item;
        if (it != rmap.end()) {
            item = rmap[key];
        } 
        mlock.unlock();
        return item;
    }
    private:
    std::map<T, U> rmap;
    std::mutex mutex_;
}
#endif

class Stream;//forward
struct RequestIdentity {
    int cnt;
    int expectedSize;
    std::shared_ptr<Stream> stream;
};

class RequestMap {
public:
    void put(const int & key, const struct RequestIdentity & value)
    {
        std::unique_lock<std::mutex> mlock(mutex_);
        rmap[key] = value;
        mlock.unlock();
    }
    struct RequestIdentity get(const int & key) 
    {
        std::unique_lock<std::mutex> mlock(mutex_);
        auto it = rmap.find(key);
        struct RequestIdentity item;
        if (it != rmap.end()) {
            item = rmap[key];
        } 
        mlock.unlock();
        return item;
    }
    int decrementCnt(const int & key)
    {
        std::unique_lock<std::mutex> mlock(mutex_);
        int cnt = (--rmap[key].cnt);
        mlock.unlock();
        return cnt;
    }
    int  incrementSize(const int & key, int size)
    {
        std::unique_lock<std::mutex> mlock(mutex_);
        rmap[key].expectedSize += size;
        int expectedSize = rmap[key].expectedSize;
        mlock.unlock();
        return expectedSize;
    }
    int decrementSize(const int & key, int size)
    {
        std::unique_lock<std::mutex> mlock(mutex_);
        rmap[key].expectedSize -= size;
        int expectedSize = rmap[key].expectedSize;
        mlock.unlock();
        return expectedSize;
    }
    std::shared_ptr<Stream> getStream(const int & key) 
    {
        std::unique_lock<std::mutex> mlock(mutex_);
        std::shared_ptr<Stream> st = rmap[key].stream;
        mlock.unlock();
        return st;
    }
private:
    std::map<int, struct RequestIdentity> rmap;
    std::mutex mutex_;
};


#endif /*_REQUEST_MAPPER_H_INCLUDED_*/
