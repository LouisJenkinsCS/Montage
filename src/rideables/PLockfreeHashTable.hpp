#ifndef P_LOCKFREE_HASHTABLE
#define P_LOCKFREE_HASHTABLE

#include <stdio.h>
#include <stdlib.h>

#include <iostream>
#include <atomic>
#include <algorithm>
#include <functional>
#include <vector>
#include <utility>

#include "HarnessUtils.hpp"
#include "ConcurrentPrimitives.hpp"
#include "RMap.hpp"
#include "RCUTracker.hpp"
#include "CustomTypes.hpp"
#include "PersistFunc.hpp"

// (Non-buffered) durably linearizable lock-free hash table
using namespace persist_func;
class PLockfreeHashTable : public RMap<std::string,std::string>{
    template <class T>
    class my_alloc {
    public:
        typedef T value_type;
        typedef std::size_t size_type;
        typedef T& reference;
        typedef const T& const_reference;

        T* allocate(std::size_t n) {
            // assert(alloc != NULL);
            if (auto p = static_cast<T*>(RP_malloc(n * sizeof(T)))) return p;
            throw std::bad_alloc();
        }

        void deallocate(T *ptr, std::size_t) noexcept {
            // assert(alloc != NULL);
            RP_free(ptr);
        }
    };
    struct Node;

    struct MarkPtr{
        std::atomic<Node*> ptr;
        MarkPtr(Node* n):ptr(n){};
        MarkPtr():ptr(nullptr){};
    };

    struct Node{
        std::basic_string<char,char_traits<char>,my_alloc<char>> key;
        std::basic_string<char,char_traits<char>,my_alloc<char>> val;
        MarkPtr next;
        Node(std::string k, std::string v, Node* n):key(k),val(v),next(n){
            clwb_range_nofence(key.data(), key.size());
            clwb_range_nofence(val.data(), val.size());
        };
        void* operator new(size_t size){
            // cout<<"persistent allocator called."<<endl;
            // void* ret = malloc(size);
            void* ret = RP_malloc(size);
            if (!ret){
                cerr << "Persistent::new failed: no free memory" << endl;
                exit(1);
            }
            return ret;
        }

        void* operator new (std::size_t size, void* ptr) {
            return ptr;
        }

        void operator delete(void * p) { 
            RP_free(p); 
        } 
    };
private:
    std::hash<std::string> hash_fn;
    const int idxSize=1000000;//number of buckets for hash table
    padded<MarkPtr>* buckets=new padded<MarkPtr>[idxSize]{};
    bool findNode(MarkPtr* &prev, Node* &curr, Node* &next, std::string key, int tid);

    RCUTracker<Node> tracker;

    const uint64_t MARK_MASK = ~0x1;
    inline Node* getPtr(Node* mptr){
        return (Node*) ((uint64_t)mptr & MARK_MASK);
    }
    inline bool getMark(Node* mptr){
        return (bool)((uint64_t)mptr & 1);
    }
    inline Node* mixPtrMark(Node* ptr, bool mk){
        return (Node*) ((uint64_t)ptr | mk);
    }
    inline Node* setMark(Node* mptr){
        return mixPtrMark(mptr,true);
    }
public:
    PLockfreeHashTable(int task_num) : tracker(task_num, 100, 1000, true) {
        Persistent::init();
    };
    ~PLockfreeHashTable(){
        Persistent::finalize();
    };

    void init_thread(GlobalTestConfig* gtc, LocalTestConfig* ltc){
        Persistent::init_thread(gtc, ltc);
    }

    optional<std::string> get(std::string key, int tid);
    optional<std::string> put(std::string key, std::string val, int tid);
    bool insert(std::string key, std::string val, int tid);
    optional<std::string> remove(std::string key, int tid);
    optional<std::string> replace(std::string key, std::string val, int tid);
};

class PLockfreeHashTableFactory : public RideableFactory{
    Rideable* build(GlobalTestConfig* gtc){
        return new PLockfreeHashTable(gtc->task_num);
    }
};


//-------Definition----------
optional<std::string> PLockfreeHashTable::get(std::string key, int tid) {
    MarkPtr* prev=nullptr;
    Node* curr=nullptr;
    Node* next=nullptr;
    optional<std::string> res={};

    tracker.start_op(tid);
    if(findNode(prev,curr,next,key,tid)) {
        res=curr->val;
    }
    tracker.end_op(tid);

    return res;
}

optional<std::string> PLockfreeHashTable::put(std::string key, std::string val, int tid) {
    Node* tmpNode = nullptr;
    MarkPtr* prev=nullptr;
    Node* curr=nullptr;
    Node* next=nullptr;
    optional<std::string> res={};
    tmpNode = new Node(key, val, nullptr);

    tracker.start_op(tid);
    while(true) {
        if(findNode(prev,curr,next,key,tid)) {
            // exists; replace
            res=curr->val;
            tmpNode->next.ptr.store(curr);
            clwb(tmpNode);
            sfence();
            if(prev->ptr.compare_exchange_strong(curr,tmpNode)) {
                clwb(prev);
                sfence();
                // mark curr; since findNode only finds the first node >= key, it's ok to have duplicated keys temporarily
                while(!curr->next.ptr.compare_exchange_strong(next,setMark(next)));
                clwb(curr);
                sfence();
                if(tmpNode->next.ptr.compare_exchange_strong(curr,next)) {
                    clwb(tmpNode);
                    sfence();
                    tracker.retire(curr,tid);
                } else {
                    findNode(prev,curr,next,key,tid);
                }
                break;
            }
        }
        else {
            //does not exist; insert.
            res={};
            tmpNode->next.ptr.store(curr);
            clwb(tmpNode);
            sfence();
            if(prev->ptr.compare_exchange_strong(curr,tmpNode)) {
                clwb(prev);
                sfence();
                break;
            }
        }
    }
    tracker.end_op(tid);

    return res;
}

bool PLockfreeHashTable::insert(std::string key, std::string val, int tid){
    Node* tmpNode = nullptr;
    MarkPtr* prev=nullptr;
    Node* curr=nullptr;
    Node* next=nullptr;
    bool res=false;
    tmpNode = new Node(key, val, nullptr);

    tracker.start_op(tid);
    while(true) {
        if(findNode(prev,curr,next,key,tid)) {
            res=false;
            delete tmpNode;
            break;
        }
        else {
            //does not exist, insert.
            tmpNode->next.ptr.store(curr);
            clwb(tmpNode);
            sfence();
            if(prev->ptr.compare_exchange_strong(curr,tmpNode)) {
                clwb(prev);
                sfence();
                res=true;
                break;
            }
        }
    }
    tracker.end_op(tid);

    return res;
}

optional<std::string> PLockfreeHashTable::remove(std::string key, int tid) {
    MarkPtr* prev=nullptr;
    Node* curr=nullptr;
    Node* next=nullptr;
    optional<std::string> res={};

    tracker.start_op(tid);
    while(true) {
        if(!findNode(prev,curr,next,key,tid)) {
            res={};
            break;
        }
        res=curr->val;
        sfence();
        if(!curr->next.ptr.compare_exchange_strong(next,setMark(next))) {
            continue;
        }
        clwb(curr);
        sfence();
        if(prev->ptr.compare_exchange_strong(curr,next)) {
            clwb(prev);
            sfence();
            tracker.retire(curr,tid);
        } else {
            findNode(prev,curr,next,key,tid);
        }
        break;
    }
    tracker.end_op(tid);

    return res;
}

optional<std::string> PLockfreeHashTable::replace(std::string key, std::string val, int tid) {
    Node* tmpNode = nullptr;
    MarkPtr* prev=nullptr;
    Node* curr=nullptr;
    Node* next=nullptr;
    optional<std::string> res={};
    tmpNode = new Node(key, val, nullptr);

    tracker.start_op(tid);
    while(true){
        if(findNode(prev,curr,next,key,tid)){
            res=curr->val;
            tmpNode->next.ptr.store(curr);
            clwb(tmpNode);
            sfence();
            if(prev->ptr.compare_exchange_strong(curr,tmpNode)){
                // mark curr; since findNode only finds the first node >= key, it's ok to have duplicated keys temporarily
                clwb(prev);
                sfence();
                while(!curr->next.ptr.compare_exchange_strong(next,setMark(next)));
                clwb(curr);
                sfence();
                if(tmpNode->next.ptr.compare_exchange_strong(curr,next)) {
                    clwb(tmpNode);
                    sfence();
                    tracker.retire(curr,tid);
                } else {
                    findNode(prev,curr,next,key,tid);
                }
                break;
            }
        }
        else{//does not exist
            res={};
            delete tmpNode;
            break;
        }
    }
    tracker.end_op(tid);

    return res;
}

bool PLockfreeHashTable::findNode(MarkPtr* &prev, Node* &curr, Node* &next, std::string key, int tid){
    while(true){
        size_t idx=hash_fn(key)%idxSize;
        bool cmark=false;
        prev=&buckets[idx].ui;
        curr=getPtr(prev->ptr.load());
        clwb(prev);
        while(true){//to lock old and curr
            if(curr==nullptr) return false;
            next=curr->next.ptr.load();
            clwb(curr);
            cmark=getMark(next);
            next=getPtr(next);
            int cmp = curr->key.compare(key);
            if(prev->ptr.load()!=curr) break;//retry
            clwb(prev);
            if(!cmark) {
                if(cmp == 0) {
                    sfence();
                    return true;
                } else if (cmp > 0) {
                    return false;
                }
                prev=&(curr->next);
            } else {
                sfence();
                if(prev->ptr.compare_exchange_strong(curr,next)) {
                    clwb(prev);
                    sfence();
                    tracker.retire(curr,tid);
                } else {
                    break;//retry
                }
            }
            curr=next;
        }
    }
}

#endif
