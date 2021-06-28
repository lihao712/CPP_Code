/*
这里参考java附上个人认为的一个稍微好点的线程池需要的功能：
支持core_threads: 正常线程池应该有多少个线程
支持max_task_size: 最多允许的任务个数
支持max_threads: 可以允许的最大线程个数
支持time_out: 超时时间，多出来的线程空闲一定时间后回收掉
可获取当前线程池中空闲线程个数
可获取当前线程池中总线程个数
支持定时器功能和循环执行任务功能
start()开启线程池功能
shutdown()关闭，当前线程池里的任务需要执行完
shutdown_now()立刻关闭，线程池里的任务全部清空
*/

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

class ThreadPool {
public:
    using PoolSeconds = std::chrono::seconds;

    /** 线程池的配置
   * core_threads: 核心线程个数，线程池中最少拥有的线程个数，初始化就会创建好的线程，常驻于线程池
   *
   * max_threads: >=core_threads，当任务的个数太多线程池执行不过来时，
   * 内部就会创建更多的线程用于执行更多的任务，内部线程数不会超过max_threads
   *
   * max_task_size: 内部允许存储的最大任务个数，暂时没有使用
   *
   * time_out: Cache线程的超时时间，Cache线程指的是max_threads-core_threads的线程,
   * 当time_out时间内没有执行任务，此线程就会被自动回收
   */
    struct ThreadPoolConfig {
        int core_threads;
        int max_threads;
        int max_task_size;
        PoolSeconds time_out;
    };

    /**
   * 线程的状态：有等待、运行、停止
   */
    enum class ThreadState { kInit = 0, kWaiting = 1, kRunning = 2, kStop = 3 };

    /**
   * 线程的种类标识：标志该线程是核心线程还是Cache线程，Cache是内部为了执行更多任务临时创建出来的
   */
    enum class ThreadFlag { kInit = 0, kCore = 1, kCache = 2 };

    using ThreadPtr = std::shared_ptr<std::thread>;
    using ThreadId = std::atomic<int>;
    using ThreadStateAtomic = std::atomic<ThreadState>;
    using ThreadFlagAtomic = std::atomic<ThreadFlag>;

    /**
   * 线程池中线程存在的基本单位，每个线程都有个自定义的ID，有线程种类标识和状态
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

    //线程池的初始化
    //在构造函数中将各个成员变量都附初值，同时判断线程池的config是否合法。
    ThreadPool(ThreadPoolConfig config) : config_(config) {
        this->total_function_num_.store(0);
        this->waiting_thread_num_.store(0);

        this->thread_id_.store(0);
        this->is_shutdown_.store(false);
        this->is_shutdown_now_.store(false);

        if (IsValidConfig(config_)) {
            is_available_.store(true);
        }
        else {
            is_available_.store(false);
        }
    }

    ~ThreadPool() { ShutDown(); }

    //判断能否重启并重置线程池参数
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
    //创建核心线程数个线程，常驻于线程池，等待任务的执行，线程ID由GetNextThreadId()统一分配。
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

    //如何获取当前线程池中空闲线程的个数？
    //waitingthread_num值表示空闲线程的个数，该变量在线程循环内部会更新。
    int GetWaitingThreadSize() { return this->waiting_thread_num_.load(); }

    //如何获取当前线程池中线程的总个数？
    int GetTotalThreadSize() { return this->worker_threads_.size(); }

    //如何将任务放入线程池中执行？
    //见如下代码，将任务使用std::bind封装成std::function放入任务队列中，任务较多时内部还会判断是否有空闲线程，
    //如果没有空闲线程，会自动创建出最多(max_threads-core_threads)个Cache线程用于执行任务。
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


    int GetRunnedFuncNum() { return total_function_num_.load(); }


    //如何关闭线程？
    //这里有两个标志位，isshutdown_now置为true表示立即关闭线程，isshutdown置为true则表示先执行完队列里的任务再关闭线程池
    void ShutDown() {
        ShutDown(false);
        cout << "shutdown" << endl;
    }

    void ShutDownNow() {
        ShutDown(true);
        cout << "shutdown now" << endl;
    }

private:
    void ShutDown(bool is_now) {
        if (is_available_.load()) {
            if (is_now) {
                this->is_shutdown_now_.store(true);
            }
            else {
                this->is_shutdown_.store(true);
            }
            this->task_cv_.notify_all();
            is_available_.store(false);
        }
    }

    bool IsAvailable() { return is_available_.load(); }


    //如何为线程池添加线程？
    //见AddThread()函数，默认会创建Core线程，也可以选择创建Cache线程，
    //线程内部会有一个死循环，不停的等待任务，有任务到来时就会执行，同时内部会判断是否是Cache线程，
    //如果是Cache线程，timeout时间内没有任务执行就会自动退出循环，线程结束。
    //这里还会检查is_shutdown和isshutdown_now标志，根据两个标志位是否为true来判断是否结束线程。
    void AddThread(int id) { AddThread(id, ThreadFlag::kCore); }

    void AddThread(int id, ThreadFlag thread_flag) {
        cout << "AddThread " << id << " flag " << static_cast<int>(thread_flag) << endl;
        ThreadWrapperPtr thread_ptr = std::make_shared<ThreadWrapper>();
        thread_ptr->id.store(id);
        thread_ptr->flag.store(thread_flag);
        auto func = [this, thread_ptr]() {
            for (;;) {
                std::function<void()> task;
                {
                    ThreadPoolLock lock(this->task_mutex_);
                    if (thread_ptr->state.load() == ThreadState::kStop) {
                        break;
                    }
                    cout << "thread id " << thread_ptr->id.load() << " running start" << endl;
                    thread_ptr->state.store(ThreadState::kWaiting);
                    ++this->waiting_thread_num_;
                    bool is_timeout = false;
                    if (thread_ptr->flag.load() == ThreadFlag::kCore) {
                        //如果lambda为false，那么流程如上，又休眠，再等待唤醒；如果表达式为true，那么wait返回，流程走下来
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
                    //如果
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
                    //从任务列表中取出一个任务，交由此线程执行
                    thread_ptr->state.store(ThreadState::kRunning);
                    task = std::move(this->tasks_.front());
                    this->tasks_.pop();
                }
                //开始运行任务，根据main的定义，会打印function...，之后睡眠
                task();
            }
            cout << "thread id " << thread_ptr->id.load() << " running end" << endl;
        };
        thread_ptr->ptr = std::make_shared<std::thread>(std::move(func));
        if (thread_ptr->ptr->joinable()) {
            thread_ptr->ptr->detach();
        }
        this->worker_threads_.emplace_back(std::move(thread_ptr));
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

    bool IsValidConfig(ThreadPoolConfig config) {
        if (config.core_threads < 1 || config.max_threads < config.core_threads || config.time_out.count() < 1) {
            return false;
        }
        return true;
    }

private:
    ThreadPoolConfig config_;

    std::list<ThreadWrapperPtr> worker_threads_;

    std::queue<std::function<void()>> tasks_;
    std::mutex task_mutex_;
    std::condition_variable task_cv_;

    std::atomic<int> total_function_num_;
    std::atomic<int> waiting_thread_num_;
    std::atomic<int> thread_id_;

    std::atomic<bool> is_shutdown_now_;
    std::atomic<bool> is_shutdown_;
    std::atomic<bool> is_available_;
};

int main() {
    cout << "hello" << endl;
    //初始化线程池，核心线程4，最大线程5，最大任务个数6，超时时间为4秒
    ThreadPool pool( ThreadPool::ThreadPoolConfig{ 4, 5, 6, std::chrono::seconds(4) } );
    //开启线程池，创建4个核心线程，更新线程池的计数，之后睡眠，等待条件变量的唤醒
    pool.Start();
    //std::this_thread是一个命名空间，包含一系列访问当前线程的函数，sleep_for是阻塞线程到指定时间段4秒之后，保证线程池开启成功
    std::this_thread::sleep_for(std::chrono::seconds(4));
    //得到此时线程池中的存在线程的个数，此处应为4
    cout << "thread size " << pool.GetTotalThreadSize() << endl;
    //新建指示值
    std::atomic<int> index;
    index.store(0);
    //创建新线程，执行lamda表达式，运行10次Run函数，即将任务使用std::bind封装成std::function放入任务队列中
    std::thread t([&]() {
        for (int i = 0; i < 10; ++i) {
            pool.Run([&]() {
                cout << "function " << index.load() << endl;
                std::this_thread::sleep_for(std::chrono::seconds(4));
                index++;
                });
            // std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        });
    t.detach(); //分离主线程和t线程，防止阻塞
    
    cout << "=================" << endl;
    //主线程睡眠4秒，等待之前的任务完成
    std::this_thread::sleep_for(std::chrono::seconds(4));
    //更新线程池，核心线程4，最大线程4，最大任务个数6，超时时间为4秒（之前启动的Cache在超时后会自动退出循环，线程结束
    pool.Reset(ThreadPool::ThreadPoolConfig{ 4, 4, 6, std::chrono::seconds(4) });

    std::this_thread::sleep_for(std::chrono::seconds(4));
    cout << "thread size " << pool.GetTotalThreadSize() << endl;
    cout << "waiting size " << pool.GetWaitingThreadSize() << endl;
    cout << "---------------" << endl;
    pool.ShutDownNow();
    
    cout << "world" << endl;
    return 0;
}