#ifndef __NONCOPYABLE__
#define __NONCOPYABLE__

#include <iostream>

namespace wzq {

    class NonCopyAble {
    public:
        NonCopyAble(const NonCopyAble&) = delete;
        NonCopyAble& operator=(const NonCopyAble&) = delete;

    protected: // ��ֹ��̬����ʱdelete ����ָ��
        NonCopyAble() = default;
        ~NonCopyAble() = default;
    };

}  // namespace wzq

#endif