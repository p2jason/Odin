// thread_pool.cpp

#define WORKER_TASK_PROC(name) isize name(void *data)
typedef WORKER_TASK_PROC(WorkerTaskProc);

#if defined(GB_SYSTEM_WINDOWS)
#define THREAD_POOL_SYNC_FETCH_AND_ADD InterlockedAdd
#else
#define THREAD_POOL_SYNC_FETCH_AND_ADD __sync_fetch_and_add
#endif

struct WorkerTask {
	WorkerTask *next_task;
	WorkerTaskProc *do_work;
	void *data;
};

struct ThreadPool {
#if defined(GB_SYSTEM_WINDOWS)
	volatile LONG outstanding_task_count;
#else
	volatile isize outstanding_task_count;
#endif
	WorkerTask *next_task;
	BlockingMutex task_list_mutex;
};

void thread_pool_init(ThreadPool *pool, gbAllocator const &a, isize thread_count, char const *worker_prefix = nullptr);
void thread_pool_destroy(ThreadPool *pool);
void thread_pool_wait(ThreadPool *pool);
void thread_pool_add_task(ThreadPool *pool, WorkerTaskProc *proc, void *data);
void worker_thread_internal();

void thread_pool_thread_entry(ThreadPool *pool) {
	while (pool->outstanding_task_count) {
		mutex_lock(&pool->task_list_mutex);

		if (pool->next_task) {
			WorkerTask *task = pool->next_task;
			pool->next_task = task->next_task;
			mutex_unlock(&pool->task_list_mutex);
			task->do_work(task->data);
			THREAD_POOL_SYNC_FETCH_AND_ADD(&pool->outstanding_task_count, -1);
			free(task);
		} else {
			mutex_unlock(&pool->task_list_mutex);
			yield();
		}
	}
}

#if defined(GB_SYSTEM_WINDOWS)
	DWORD __stdcall thread_pool_thread_entry_platform(void *arg) {
		thread_pool_thread_entry((ThreadPool *) arg);
		return 0;
	}

	void thread_pool_start_thread(ThreadPool *pool) {
		CloseHandle(CreateThread(NULL, 0, thread_pool_thread_entry_platform, pool, 0, NULL));
	}
#else
	void *thread_pool_thread_entry_platform(void *arg) {
		thread_pool_thread_entry((ThreadPool *) arg);
		return NULL;
	}

	void thread_pool_start_thread(ThreadPool *pool) {
		pthread_t handle;
		pthread_create(&handle, NULL, thread_pool_thread_entry_platform, pool);
		pthread_detach(handle);
	}
#endif

void thread_pool_init(ThreadPool *pool, gbAllocator const &a, isize thread_count, char const *worker_prefix) {
	memset(pool, 0, sizeof(ThreadPool));
	mutex_init(&pool->task_list_mutex);
	pool->outstanding_task_count = 1;

	for (int i = 0; i < thread_count; i++) {
		thread_pool_start_thread(pool);
	}
}

void thread_pool_destroy(ThreadPool *pool) {
	mutex_destroy(&pool->task_list_mutex);
}

void thread_pool_wait(ThreadPool *pool) {
	THREAD_POOL_SYNC_FETCH_AND_ADD(&pool->outstanding_task_count, -1);

	while (pool->outstanding_task_count) {
		yield();
	}
}

void thread_pool_add_task(ThreadPool *pool, WorkerTaskProc *proc, void *data) {
	WorkerTask *task = (WorkerTask *) calloc(1, sizeof(WorkerTask));
	task->do_work = proc;
	task->data = data;
	mutex_lock(&pool->task_list_mutex);
	task->next_task = pool->next_task;
	pool->next_task = task;
	THREAD_POOL_SYNC_FETCH_AND_ADD(&pool->outstanding_task_count, 1);
	mutex_unlock(&pool->task_list_mutex);
}
