// Decompiled from TASK.c: the game's timed callbacks. Tasks wait in a list sorted by time between a header (time -1)
// and a footer (time 0x7fffffff); TaskProcess calls every task that has fallen due. Flag 1 marks a task in the list.
// pInsert remembers the last insertion so that TaskCachedInsert can search from there.

#include "mem_fuzz.h"

namespace rc {

namespace {

constexpr uint32_t kTaskVars = 0x31ff58u;

ptr32<_sTask> taskAt(uint32_t offset) { return ptr32<_sTask>(kTaskVars + offset); }
ptr32<_sTask> header() { return taskAt(0xcu); }
ptr32<_sTask> footer() { return taskAt(0x24u); }

// The original's callee-saved registers while a callback runs ($s0 the task, $s1 the task globals, $s2 the time), so a
// callback that reads its caller's registers finds the same values.
class HeldTaskRegisters
{
public:
    HeldTaskRegisters(uint32_t s0, uint32_t s1, uint32_t s2) : m_ctx(g_runtime->eeScheduler().runningContext())
    {
        for (int i = 0; i < 3; ++i)
            m_saved[i] = m_ctx.r[16 + i];
        set(s0, s1, s2);
    }
    void set(uint32_t s0, uint32_t s1, uint32_t s2)
    {
        SET_GPR_S32((&m_ctx), 16, static_cast<int32_t>(s0));
        SET_GPR_S32((&m_ctx), 17, static_cast<int32_t>(s1));
        SET_GPR_S32((&m_ctx), 18, static_cast<int32_t>(s2));
    }
    ~HeldTaskRegisters()
    {
        for (int i = 0; i < 3; ++i)
            m_ctx.r[16 + i] = m_saved[i];
    }
    HeldTaskRegisters(const HeldTaskRegisters &) = delete;
    HeldTaskRegisters &operator=(const HeldTaskRegisters &) = delete;

private:
    R5900Context &m_ctx;
    std::remove_reference_t<decltype(std::declval<R5900Context &>().r[0])> m_saved[3];
};

} // namespace

void TaskInit()
{
    taskClass &tasks = G(taskVarG);
    tasks.bInitialised = 1;
    tasks.footer.pNext = header();
    tasks.header.time = -1;
    tasks.header.pNext = footer();
    tasks.footer.flags = 1;
    tasks.footer.time = 0x7fffffff;
    tasks.time = -1;
    tasks.pInsert = header();
    tasks.header.flags = 1;
    tasks.header.pFnCallback = ee_fnptr{0};
    tasks.header.pData = nullptr;
    tasks.header.pPrev = footer();
    tasks.footer.pFnCallback = ee_fnptr{0};
    tasks.footer.pData = nullptr;
    tasks.footer.pPrev = header();
}

void TaskTerm()
{
    taskClass &tasks = G(taskVarG);
    tasks.bInitialised = 0;
    tasks.pInsert = nullptr;
}

// Takes the first task off the list (the caller has checked it is due).
void TaskPop()
{
    taskClass &tasks = G(taskVarG);
    const ptr32<_sTask> first = tasks.header.pNext;
    const ptr32<_sTask> next = first->pNext;
    const ptr32<_sTask> insert = tasks.pInsert;
    next->pPrev = header();
    if (insert == first)
        tasks.pInsert = next;
    const uint32_t flags = first->flags;
    tasks.header.pNext = next;
    first->flags = flags & ~1u;
}

// Calls every task due by currentTime, in time order, with its data and time. A callback may insert tasks again.
void TaskProcess(tTime currentTime)
{
    taskClass &tasks = G(taskVarG);
    GuestFrame frame(0x40);
    HeldTaskRegisters held(0, kTaskVars, static_cast<uint32_t>(currentTime));
    for (;;)
    {
        const ptr32<_sTask> task = tasks.header.pNext;
        held.set(task.address, kTaskVars, static_cast<uint32_t>(currentTime));
        if (currentTime < task->time)
            return;
        TaskPop();
        const tTime time = task->time;
        const uint32_t callback = task->pFnCallback.address;
        tasks.time = time;
        guest_call(callback, task->pData, task->time);
    }
}

void TaskSet(ptr32<_sTask> pTask, ee_fnptr pFnCallback, ptr32<void> pData, tTime time)
{
    pTask->pData = pData;
    pTask->pFnCallback = pFnCallback;
    pTask->time = time;
    pTask->flags = 0;
}

// Inserts after every task with an earlier time, searching from the header.
void TaskInsert(ptr32<_sTask> pTask)
{
    taskClass &tasks = G(taskVarG);
    const tTime time = pTask->time;
    ptr32<_sTask> prev = header();
    const uint32_t flags = pTask->flags;
    while (prev->pNext->time < time)
        prev = prev->pNext;
    pTask->pPrev = prev;
    pTask->flags = flags | 1u;
    const ptr32<_sTask> next = prev->pNext;
    tasks.pInsert = pTask;
    pTask->pNext = next;
    prev->pNext->pPrev = pTask;
    prev->pNext = pTask;
}

// As TaskInsert, searching from the last insertion: backwards past later tasks, or forwards past earlier ones.
void TaskCachedInsert(ptr32<_sTask> pTask)
{
    taskClass &tasks = G(taskVarG);
    const tTime time = pTask->time;
    ptr32<_sTask> at = tasks.pInsert;
    uint32_t flags;
    if (time < at->time)
    {
        // Goes before at: back while the task before is later still.
        ptr32<_sTask> prev = at->pPrev;
        flags = pTask->flags;
        while (time < prev->time)
        {
            at = prev;
            prev = at->pPrev;
        }
        pTask->pPrev = prev;
        pTask->pNext = at;
        at->pPrev->pNext = pTask;
        at->pPrev = pTask;
    }
    else
    {
        flags = pTask->flags;
        while (at->pNext->time < time)
            at = at->pNext;
        pTask->pPrev = at;
        pTask->pNext = at->pNext;
        at->pNext->pPrev = pTask;
        at->pNext = pTask;
    }
    tasks.pInsert = pTask;
    pTask->flags = flags | 1u;
}

// Takes a task out of the list if it is in it; the search point moves to the task after.
void TaskDelete(ptr32<_sTask> pTask)
{
    taskClass &tasks = G(taskVarG);
    const uint32_t flags = pTask->flags;
    if ((flags & 1u) == 0)
        return;
    const ptr32<_sTask> prev = pTask->pPrev;
    const ptr32<_sTask> next = pTask->pNext;
    prev->pNext = next;
    pTask->flags = flags & ~1u;
    pTask->pNext->pPrev = pTask->pPrev;
    tasks.pInsert = pTask->pNext;
}

} // namespace rc

RC_DECOMP_FN(TaskInit, 0x1af880)
RC_DECOMP_FN(TaskTerm, 0x1af8e0)
RC_DECOMP_FN_SETUP_HEAP(TaskProcess, 0x1af8f8, rc::fuzz::taskProcess)
RC_DECOMP_FN_SETUP(TaskSet, 0x1afad8, rc::fuzz::taskSet)
RC_DECOMP_FN_SETUP_HEAP(TaskInsert, 0x1afaf0, rc::fuzz::taskInsert)
RC_DECOMP_FN_SETUP_HEAP(TaskCachedInsert, 0x1afb68, rc::fuzz::taskInsert)
RC_DECOMP_FN_SETUP_HEAP(TaskDelete, 0x1afc48, rc::fuzz::taskDelete)
RC_DECOMP_FN_SETUP_HEAP(TaskPop, 0x1afc98, rc::fuzz::taskList)
