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


//��ʱ������Ҫ�����ݽṹ

//���ȼ�������У������д洢����ÿ����������ʱ����������ʱ�����������ȳ��ӡ�
//������������������������Ҫִ��ʱ������֪ͨ���ڵȴ����̴߳����������ȡ������ִ�С�
//�̳߳أ��������������̳߳���ִ�С�

namespace wzq {
    class TimerQueue {
    public:
        //�����еĵ�Ԫ
        struct InternalS {
            std::chrono::time_point<std::chrono::high_resolution_clock> time_point_;
            std::function<void()> func_;
            int repeated_id;
            //��������أ����ڱȽ�������Ԫ��ʱ���
            bool operator<(const InternalS& b) const { return time_point_ > b.time_point_; }
        };

    public:

        //���ڲ����̳߳ع��ܣ��̳߳�����core�̣߳�����ִ�з��붨ʱ���е�����ͬʱ�¿�һ���̣߳�ѭ���ȴ��������������̳߳���ִ�С�
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

        //��ιرն�ʱ������
        //������ʹ��running_��־λ���ƣ���־λΪfalse�������̵߳�ѭ���ͻ��Զ��˳����Ͳ�������ȴ�����ִ�У�ͬʱ�̳߳�Ҳ�ر�
        void Stop() {
            running_.store(false);
            cond_.notify_all();
            thread_pool_.ShutDown();
        }

        //�����ĳ��ʱ���ִ������
        //���ݵ�ǰʱ�����ʱ��ι����ʱ����Ӷ�����InternalS����������У�
        //std::chrono::duration<R, P>& time���ͣ�R��ʾһ����ֵ���ͣ�������ʾP��������P��������ʾ�����ʾ��ʱ�䵥λ
        template <typename R, typename P, typename F, typename... Args>
        void AddFuncAfterDuration(const std::chrono::duration<R, P>& time, F&& f, Args&&... args) {
            InternalS s;
            //ʱ�����Ϊ��ǰʱ��+�����ʱ���
            s.time_point_ = std::chrono::high_resolution_clock::now() + time;
            //������ͨ��bind���з�װ
            s.func_ = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
            //��������ӽ���У����������߳̽�������ִ��
            std::unique_lock<std::mutex> lock(mutex_);
            queue_.push(s);
            cond_.notify_all();
        }

        //�����ĳһʱ���ִ������
        //����ʱ�������InternalS����������У�
        template <typename F, typename... Args>
        void AddFuncAtTimePoint(const std::chrono::time_point<std::chrono::high_resolution_clock>& time_point, F&& f,
            Args&&... args) {
            InternalS s;
            //ʱ�����Ϊ�����ʱ���
            s.time_point_ = time_point;
            //������ͨ��bind���з�װ
            s.func_ = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
            //��������ӽ���У����������߳̽�������ִ��
            std::unique_lock<std::mutex> lock(mutex_);
            queue_.push(s);
            cond_.notify_all();
        }

        //���ѭ��ִ������
        //����Ϊ���ѭ���������ɱ�ʶID���ⲿ����ͨ��ID��ȡ�����������ִ�У��������£��ڲ������Ƶݹ�ķ�ʽѭ��ִ������
        template <typename R, typename P, typename F, typename... Args>
        int AddRepeatedFunc(int repeat_num, const std::chrono::duration<R, P>& time, F&& f, Args&&... args) {
            //�õ���һ�ε��ظ�����ִ�д����ļ�¼id
            int id = GetNextRepeatedFuncId();
            //��ϣ���в�����ε�ѭ������ı�ʶ
            repeated_id_state_map_.Emplace(id, RepeatedIdState::kRunning);
            //������ͨ��bind���з�װ
            auto tem_func = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
            //���ú�������ѭ������
            AddRepeatedFuncLocal(repeat_num - 1, time, id, std::move(tem_func));
            return id;
        }

        //���ȡ��ѭ�������ִ��
        //��ʱ���ڲ���repeated_id_state_map ���ݽṹ�����ڴ洢ѭ�������ID����ȡ������ִ��ʱ������ID��repeatedid_state_map���Ƴ���ѭ������ͻ��Զ�ȡ����
        void CancelRepeatedFuncId(int func_id) { repeated_id_state_map_.EraseKey(func_id); }

        //�õ���һ�ε��ظ�����ִ�д����ļ�¼id
        int GetNextRepeatedFuncId() { return repeated_func_id_++; }

        //�ڹ��캯���г�ʼ������Ҫ�����ú��ڲ����̳߳أ��̳߳��г�פ���߳���Ŀǰ��Ϊ4����������ο�֮ǰ���̳߳����
        TimerQueue() : thread_pool_(wzq::ThreadPool::ThreadPoolConfig{ 4, 4, 40, std::chrono::seconds(4) }) {
            repeated_func_id_.store(0);
            running_.store(true);
        }

        ~TimerQueue() { Stop(); }

        enum class RepeatedIdState { kInit = 0, kRunning = 1, kStop = 2 };

    private:
        void RunLocal() {
            //ֻҪ��ʱ�������У��ͳ�����ѭ��
            while (running_.load()) {
                //�����ж϶�������û������
                std::unique_lock<std::mutex> lock(mutex_);
                //û��������ͷ�����˯��ȥ
                if (queue_.empty()) {
                    cond_.wait(lock);
                    continue;
                }
                //������͵õ���������
                auto s = queue_.top();
                //����ȡ��������͵��ڰ���ʱ���ʱ�����
                auto diff = s.time_point_ - std::chrono::high_resolution_clock::now();
                //ʱ�仹û�����ͼ����ͷ�����˯��ȥ
                if (std::chrono::duration_cast<std::chrono::milliseconds>(diff).count() > 0) {
                    cond_.wait_for(lock, diff);
                    continue;
                }
                //ʱ�䵽�ˣ���ȡ��������ӵ��̳߳ص���������У��̳߳ػỽ��һ���߳�ȥִ������
                else {
                    queue_.pop();
                    lock.unlock();
                    thread_pool_.Run(std::move(s.func_));
                }
            }
            cout << "��ʱ���ر�" << endl;
        }

        template <typename R, typename P, typename F>
        void AddRepeatedFuncLocal(int repeat_num, const std::chrono::duration<R, P>& time, int id, F&& f) {
            //���ж����ѭ��������û��ȡ��
            if (!this->repeated_id_state_map_.IsKeyExist(id)) {
                return;
            }
            //����һ�����нڵ�
            InternalS s;
            //���в�������
            s.time_point_ = std::chrono::high_resolution_clock::now() + time;
            auto tem_func = std::move(f);
            s.repeated_id = id;
            //�������˼�ǣ�ֻ���ڴ˽ڵ�ʱ�䵽��ȡ��ִ��ʱ�򣬻��������һ��ִ�к�����ͬ��ͬ���ͽڵ㣬ֻ���ظ�����-1
            s.func_ = [this, &tem_func, repeat_num, time, id]() {
                tem_func();
                if (!this->repeated_id_state_map_.IsKeyExist(id) || repeat_num == 0) {
                    return;
                }
                AddRepeatedFuncLocal(repeat_num - 1, time, id, std::move(tem_func));
            };
            //��������������ڵ㣬���������ѹ����߳�
            std::unique_lock<std::mutex> lock(mutex_);
            queue_.push(s);
            lock.unlock();
            cond_.notify_all();
        }

    private:
        std::priority_queue<InternalS> queue_;  //���ȼ�������У������д洢����ÿ����������ʱ����������ʱ�����������ȳ��ӡ�
        std::atomic<bool> running_;
        std::mutex mutex_;  //����������Ҫִ��ʱ������֪ͨ���ڵȴ����̴߳����������ȡ������ִ�С�
        std::condition_variable cond_;

        wzq::ThreadPool thread_pool_;  //�������������̳߳���ִ�С�

        std::atomic<int> repeated_func_id_;  //��ʾѭ������ĵ�ǰִ������
        wzq::ThreadSafeMap<int, RepeatedIdState> repeated_id_state_map_;  //��ϣ�����ڼ�¼ѭ�������ִ��״̬
    };

} 

#endif