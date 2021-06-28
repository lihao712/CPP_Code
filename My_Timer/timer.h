#ifndef __TIMER__
#define __TIMER__

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>

#include "my_map.h"
#include "thread_pool.h"


//定时器中主要的数据结构

//优先级任务队列：队列中存储任务，每个任务会添加时间戳，最近的时间戳的任务会先出队。
//锁和条件变量：当有任务需要执行时，用于通知正在等待的线程从任务队列中取出任务执行。
//线程池：各个任务会放在线程池中执行。

namespace wzq {
    class TimerQueue {
    public:
        //队列中的单元
        struct InternalS {
            std::chrono::time_point<std::chrono::high_resolution_clock> time_point_;
            std::function<void()> func_;
            int repeated_id;
            //运算符重载，用于比较两个单元的时间戳
            bool operator<(const InternalS& b) const { return time_point_ > b.time_point_; }
        };

    public:

        //打开内部的线程池功能，线程池生成core线程，用于执行放入定时器中的任务，同时新开一个线程，循环等待任务到来后送入线程池中执行。
        bool Run() {
            bool ret = thread_pool_.Start();
            if (!ret) {
                return false;
            }
            std::thread([this]() { RunLocal(); }).detach();
            return true;
        }

        bool IsAvailable() { return thread_pool_.IsAvailable(); }

        int Size() { return queue_.size(); }

        //如何关闭定时器功能
        //这里是使用running_标志位控制，标志位为false，调度线程的循环就会自动退出，就不会继续等待任务执行，同时线程池也关闭
        void Stop() {
            running_.store(false);
            cond_.notify_all();
            thread_pool_.ShutDown();
        }

        //如何在某段时间后执行任务
        //根据当前时间加上时间段构造出时间戳从而构造InternalS，放入队列中：
        //std::chrono::duration<R, P>& time解释：R表示一种数值类型，用来表示P的数量，P是用来表示用秒表示的时间单位
        template <typename R, typename P, typename F, typename... Args>
        void AddFuncAfterDuration(const std::chrono::duration<R, P>& time, F&& f, Args&&... args) {
            InternalS s;
            //时间戳记为当前时间+传入的时间段
            s.time_point_ = std::chrono::high_resolution_clock::now() + time;
            //将函数通过bind进行封装
            s.func_ = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
            //上锁，添加金队列，唤醒所有线程进行抢锁执行
            std::unique_lock<std::mutex> lock(mutex_);
            queue_.push(s);
            cond_.notify_all();
        }

        //如何在某一时间点执行任务
        //根据时间戳构造InternalS，放入队列中：
        template <typename F, typename... Args>
        void AddFuncAtTimePoint(const std::chrono::time_point<std::chrono::high_resolution_clock>& time_point, F&& f,
            Args&&... args) {
            InternalS s;
            //时间戳记为传入的时间点
            s.time_point_ = time_point;
            //将函数通过bind进行封装
            s.func_ = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
            //上锁，添加金队列，唤醒所有线程进行抢锁执行
            std::unique_lock<std::mutex> lock(mutex_);
            queue_.push(s);
            cond_.notify_all();
        }

        //如何循环执行任务
        //首先为这个循环任务生成标识ID，外部可以通过ID来取消此任务继续执行，代码如下，内部以类似递归的方式循环执行任务
        template <typename R, typename P, typename F, typename... Args>
        int AddRepeatedFunc(int repeat_num, const std::chrono::duration<R, P>& time, F&& f, Args&&... args) {
            //得到下一次的重复任务执行次数的记录id
            int id = GetNextRepeatedFuncId();
            //哈希表中插入这次的循环任务的标识
            repeated_id_state_map_.Emplace(id, RepeatedIdState::kRunning);
            //将函数通过bind进行封装
            auto tem_func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
            //调用函数生成循环任务
            AddRepeatedFuncLocal(repeat_num - 1, time, id, std::move(tem_func));
            return id;
        }

        //如何取消循环任务的执行
        //定时器内部有repeated_id_state_map 数据结构，用于存储循环任务的ID，当取消任务执行时，将此ID从repeatedid_state_map中移除，循环任务就会自动取消。
        void CancelRepeatedFuncId(int func_id) { repeated_id_state_map_.EraseKey(func_id); }

        //得到下一次的重复任务执行次数的记录id
        int GetNextRepeatedFuncId() { return repeated_func_id_++; }

        //在构造函数中初始化，主要是配置好内部的线程池，线程池中常驻的线程数目前设为4，具体分析参考之前的线程池设计
        TimerQueue() : thread_pool_(wzq::ThreadPool::ThreadPoolConfig{ 4, 4, 40, std::chrono::seconds(4) }) {
            repeated_func_id_.store(0);
            running_.store(true);
        }

        ~TimerQueue() { Stop(); }

        enum class RepeatedIdState { kInit = 0, kRunning = 1, kStop = 2 };

    private:
        void RunLocal() {
            //只要定时器在运行，就持续的循环
            while (running_.load()) {
                //上锁判断队列中有没有任务
                std::unique_lock<std::mutex> lock(mutex_);
                //没有任务就释放锁，睡过去
                if (queue_.empty()) {
                    cond_.wait(lock);
                    continue;
                }
                //有任务就得到队首任务
                auto s = queue_.top();
                //计算取出的任务和档期啊那时间的时间戳差
                auto diff = s.time_point_ - std::chrono::high_resolution_clock::now();
                //时间还没到，就继续释放锁，睡过去
                if (std::chrono::duration_cast<std::chrono::milliseconds>(diff).count() > 0) {
                    cond_.wait_for(lock, diff);
                    continue;
                }
                //时间到了，就取出任务，添加到线程池的任务队列中，线程池会唤醒一个线程去执行任务
                else {
                    queue_.pop();
                    lock.unlock();
                    thread_pool_.Run(std::move(s.func_));
                }
            }
            cout << "定时器关闭" << endl;
        }

        template <typename R, typename P, typename F>
        void AddRepeatedFuncLocal(int repeat_num, const std::chrono::duration<R, P>& time, int id, F&& f) {
            //先判断这个循环任务有没有取消
            if (!this->repeated_id_state_map_.IsKeyExist(id)) {
                return;
            }
            //生成一个队列节点
            InternalS s;
            //进行参数配置
            s.time_point_ = std::chrono::high_resolution_clock::now() + time;
            auto tem_func = std::move(f);
            s.repeated_id = id;
            //这里的意思是：只有在此节点时间到别取出执行时候，会继续推入一个执行函数相同的同类型节点，只是重复次数-1
            s.func_ = [this, &tem_func, repeat_num, time, id]() {
                tem_func();
                if (!this->repeated_id_state_map_.IsKeyExist(id) || repeat_num == 0) {
                    return;
                }
                AddRepeatedFuncLocal(repeat_num - 1, time, id, std::move(tem_func));
            };
            //队列上锁，加入节点，解锁并唤醒工作线程
            std::unique_lock<std::mutex> lock(mutex_);
            queue_.push(s);
            lock.unlock();
            cond_.notify_all();
        }

    private:
        std::priority_queue<InternalS> queue_;  //优先级任务队列：队列中存储任务，每个任务会添加时间戳，最近的时间戳的任务会先出队。
        std::atomic<bool> running_;
        std::mutex mutex_;  //当有任务需要执行时，用于通知正在等待的线程从任务队列中取出任务执行。
        std::condition_variable cond_;

        wzq::ThreadPool thread_pool_;  //各个任务会放在线程池中执行。

        std::atomic<int> repeated_func_id_;  //表示循环任务的当前执行任务
        wzq::ThreadSafeMap<int, RepeatedIdState> repeated_id_state_map_;  //哈希表，用于记录循环任务的执行状态
    };

} 

#endif