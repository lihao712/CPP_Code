/*线程池中主要的数据结构：
1. 链表或者数组：用于存储线程池中的线程。

2. 队列：用于存储需要放入线程池中执行的任务。

3. 条件变量：当有任务需要执行时，用于通知正在等待的线程从任务队列中取出任务执行。

代码如下：
*/

#ifndef __THREAD_POOL__
#define __THREAD_POOL__

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using std::cout;
using std::endl;

namespace wzq {

    class ThreadPool {
    public:
        using PoolSeconds = std::chrono::seconds;

        /** 线程池的配置
         * core_threads:核心线程个数，线程池中拥有的最小线程个数，初始化就会创建好的线程，常驻与线程池
         *
         * max_threads: >= core_threads，当任务的个数太多线程池执行不过来时，内部就会创建更多的线程用于执行更多的任务
         * 内部线程数不会超过max_threads
         *
         * max_task_size: 内部允许存储的最大任务个数，暂时没有使用
         *
         * time_out: Cache线程的超时时间，Cache线程指的是max_threads-core_threads的线程，当time_out时间内没有执行任务，
         * 此线程就被自动回收
         */
        struct ThreadPoolConfig {
            int core_threads;
            int max_threads;
            int max_task_size;
            PoolSeconds time_out;
        };

        /**
         * 线程的状态：有等待，运行，停止
         */
        enum class ThreadState { kInit = 0, kWaiting = 1, kRunning = 2, kStop = 3 };

        /**
         * 线程的种类标识：标志该线程时核心线程还是Cache线程，Cache是内部为了执行更多任务临时创建出来的
         */
        enum class ThreadFlag { kInit = 0, kCore = 1, kCache = 2 };

        using ThreadPtr = std::shared_ptr<std::thread>;
        using ThreadId = std::atomic<int>;
        using ThreadStateAtomic = std::atomic<ThreadState>;
        using ThreadFlagAtomic = std::atomic<ThreadFlag>;

        /**
         * 线程池中存在的基本单位，每个线程都有个自定义ID，有线程种类标识和状态
         */
        struct ThreadWrapper {
            ThreadPtr ptr;
            ThreadId id;
            ThreadFlagAtomic flag;
            ThreadStateAtomic state;


            ThreadWrapper() {
                ptr = nullptr;
                id = 0;
                state.store(ThreadState::kInit);
            }
        };

        using ThreadWrapperPtr = std::shared_ptr<ThreadWrapper>;
        using ThreadPoolLock = std::unique_lock<std::mutex>;

        //线程池的初始化:
        //在构造函数中将各个成员变量都附初值
        ThreadPool(ThreadPoolConfig config) : config_(config) {
            this->total_function_num_.store(0);
            this->waiting_thread_num_.store(0);
            this->thread_id_.store(0);
            this->is_shutdown_.store(false);
            this->is_shutdown_now_.store(false);
            if (IsValidConfig(config_)) {
                is_aviailable_.store(true);
            }
            else {
                is_aviailable_.store(false);
            }
        }

        ~ThreadPool() { ShutDown(); }
           
        //重启线程池
         bool Reset(ThreadPoolConfig config) {
            if (!IsValidConfig(config)) {
                return false;
            }
            if (config_.core_threads != config.core_threads) {
                return false;
            }
            config_ = config;
            return true;
         }

        

        //如何开启线程池功能？
        //创建核心线程数个线程，常驻于线程池，等待人物的执行，线程ID由GetNextThreadId()统一分配
        bool Start() {
            if (!IsAvailable()) {
                return false;
            }
            int core_thread_num = config_.core_threads;
            cout << "Init thread num " << core_thread_num << endl;
            while (core_thread_num-- > 0) {
                AddThread(GetNextThreadId());
            }
            cout << "Init thread end" << endl;
            return true;
        }

        //如何获取当前线程池中线程的总个数？
        int GetTotalThreadSize() { return this->worker_threads_.size(); }

        //如何获取当前线程池中空闲线程的个数？
        int GetWaitingThreadSize() { return this->waiting_thread_num_.load(); }

        //如何将任务放入线程池中执行？
        //见如下代码，将任务使用std::bind封装成std::function放入任务队列中，
        //任务较多时内部还会判断是否有空闲线程，如果没有空闲线程，会自动创建出最多(max_threads-core_threads)个Cache线程用于执行任务。

        //放在线程池中执行函数
        template <typename F, typename... Args>
        auto Run(F&& f, Args &&... args) -> std::shared_ptr<std::future<std::result_of_t<F(Args...)>>> {
            if (this->is_shutdown_.load() || this->is_shutdown_now_.load() || !IsAvailable()) {
                return nullptr;
            }
            if (GetWaitingThreadSize() == 0 && GetTotalThreadSize() < config_.max_threads) {
                AddThread(GetNextThreadId(), ThreadFlag::kCache);
            }
            using return_type = std::result_of_t<F(Args...)>;
            auto task = std::make_shared<std::packaged_task<return_type()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...));
            total_function_num_++;

            std::future<return_type> res = task->get_future();
            {
                ThreadPoolLock lock(this->task_mutex_);
                this->tasks_.emplace([task]() { (*task)(); });
            }
            this->task_cv_.notify_one();
            return std::make_shared<std::future<std::result_of_t<F(Args...)>>>(std::move(res));
        }


        // 获取当前线程池已经执行过的函数个数
        int GetRunnedFuncNum() { return total_function_num_.load(); }

        // 当前线程池是否可用
        bool IsAvailable() { return is_aviailable_.load(); }

        //如何关闭线程？
        //这里有两个标志位，isshutdown_now置为true表示立即关闭线程，isshutdown置为true则表示先执行完队列里的任务再关闭线程池

        //关掉线程池，内部还没有执行的任务会继续执行
        void ShutDown() {
            ShutDown(false);
            cout << "shutdown" << endl;
        }

        //执行关掉线程池，内部还没有执行的任务会直接取消，不再执行
        void ShutDownNow() {
            ShutDown(true);
            cout << "shutdown now" << endl;
        }

        

     private:
        void ShutDown(bool is_now) {
             if (is_aviailable_.load()) {
                 if (is_now) {
                     this->is_shutdown_now_.store(true);
                 }
                 else {
                     this->is_shutdown_.store(true);
                 }
                 //条件变量唤醒所有等待此条件变量的线程开始抢锁
                 this->task_cv_.notify_all();
                 is_aviailable_.store(false);
             }
        }


        //如何为线程池添加线程？
        //见AddThread()函数，默认会创建Core线程，也可以选择创建Cache线程，线程内部会有一个死循环，不停的等待任务，有任务到来时就会执行，
        //同时内部会判断是否是Cache线程，如果是Cache线程，timeout时间内没有任务执行就会自动退出循环，线程结束。

        // 这里还会检查is_shutdown和isshutdown_now标志，根据两个标志位是否为true来判断是否结束线程
        void AddThread(int id) { AddThread(id, ThreadFlag::kCore); }

        void AddThread(int id, ThreadFlag thread_flag) {
            cout << "Addthread " << id << " flag " << static_cast<int>(thread_flag) << endl;
            ThreadWrapperPtr thread_ptr = std::make_shared<ThreadWrapper>();
            thread_ptr->id.store(id);
            thread_ptr->flag.store(thread_flag);
            //使用lamda表达式创建匿名函数
            auto func = [this, thread_ptr]() {
                for (;;) {
                    std::function<void()> task; //使用函数封装器，接下来时函数的内容，应该是这样
                    {
                        ThreadPoolLock lock(this->task_mutex_); //对任务队列上锁
                        if (thread_ptr->state.load() == ThreadState::kStop) {  //线程状态为终止，退出循环
                            break;
                        }
                        cout << "thread id " << thread_ptr->id.load() << "running start" << endl;
                        thread_ptr->state.store(ThreadState::kWaiting);
                        ++this->waiting_thread_num_;
                        bool is_timeout = false;
                        //线程抢到锁后，执行相应的函数，判断此线程是否需要运行
                        if (thread_ptr->flag.load() == ThreadFlag::kCore) {
                            this->task_cv_.wait(lock, [this, thread_ptr] {
                                return (this->is_shutdown_ || this->is_shutdown_now_ || !this->tasks_.empty() ||
                                    thread_ptr->state.load() == ThreadState::kStop);
                                });
                        }
                        else {
                            this->task_cv_.wait_for(lock, this->config_.time_out, [this, thread_ptr] {
                                return (this->is_shutdown_ || this->is_shutdown_now_ || !this->tasks_.empty() ||
                                    thread_ptr->state.load() == ThreadState::kStop);
                                });
                            is_timeout = !(this->is_shutdown_ || this->is_shutdown_now_ || !this->tasks_.empty() ||
                                thread_ptr->state.load() == ThreadState::kStop);
                        }
                        --this->waiting_thread_num_;
                        cout << "thread id " << thread_ptr->id.load() << " running wait end" << endl;
                        if (is_timeout) {
                            thread_ptr->state.store(ThreadState::kStop);
                        }
                        if (thread_ptr->state.load() == ThreadState::kStop) {
                            cout << "thread id " << thread_ptr->id.load() << " state stop" << endl;
                            break;
                        }
                        if (this->is_shutdown_ && this->tasks_.empty()) {
                            cout << "thread id " << thread_ptr->id.load() << " shutdown" << endl;
                            break;
                        }
                        if (this->is_shutdown_now_) {
                            cout << "thread id " << thread_ptr->id.load() << " shutdown now" << endl;
                            break;
                        }
                        //如果线程可以运行，就改变它的状态，并取出任务队列中的一个任务分配给他
                        thread_ptr->state.store(ThreadState::kRunning);
                        task = std::move(this->tasks_.front());
                        this->tasks_.pop();
                    }
                    task(); 
                }
                cout << "thread id " << thread_ptr->id.load() << " running end" << endl;
            };
            thread_ptr->ptr = std::make_shared<std::thread>(std::move(func));
            if (thread_ptr->ptr->joinable()) {
                thread_ptr->ptr->detach();
            }
            this->worker_threads_.emplace_back(std::move(thread_ptr));  //加入工作线程列表
        }

        void Resize(int thread_num) {
            if (thread_num < config_.core_threads) return;
            int old_thread_num = worker_threads_.size();
            cout << "old num " << old_thread_num << " resize " << thread_num << endl;
            if (thread_num > old_thread_num) {
                while (thread_num-- > old_thread_num) {
                    AddThread(GetNextThreadId());
                }
            }
            else {
                int diff = old_thread_num - thread_num;
                auto iter = worker_threads_.begin();
                while (iter != worker_threads_.end()) {
                    if (diff == 0) {
                        break;
                    }
                    auto thread_ptr = *iter;
                    if (thread_ptr->flag.load() == ThreadFlag::kCache &&
                        thread_ptr->state.load() == ThreadState::kWaiting) {  // wait
                        thread_ptr->state.store(ThreadState::kStop);          // stop;
                        --diff;
                        iter = worker_threads_.erase(iter);
                    }
                    else {
                        ++iter;
                    }
                }
                this->task_cv_.notify_all();
            }
        }

        int GetNextThreadId() { return this->thread_id_++; }

        //判断线程池的ThreadPoolConfig结构体是否合法
        bool IsValidConfig(ThreadPoolConfig config) {
            if (config.core_threads < 1 || config.max_threads < config.core_threads || config.time_out.count() < 1) {
                return false;
            }
            return true;
        }
        
        

    private:
        ThreadPoolConfig config_; //定义线程池状态结构体

        std::list<ThreadWrapperPtr> worker_threads_; //定义线程列表

        std::queue<std::function<void()>> tasks_;  //定义任务队列
        std::mutex task_mutex_; //定义任务队列的控制锁
        std::condition_variable task_cv_;  //定义控制多线程的条件变量，和任务队列控制锁配合使用


        std::atomic<int> total_function_num_;
        std::atomic<int> waiting_thread_num_;
        std::atomic<int> thread_id_; //用于为新线程分配id

        std::atomic<bool> is_shutdown_now_;
        std::atomic<bool> is_shutdown_;
        std::atomic<bool> is_aviailable_;
    };
}
#endif
