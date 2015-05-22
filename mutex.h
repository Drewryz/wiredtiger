
struct __wt_condvar
{
	const char*		name;
	wt_mutex_t		mtx;
	wt_cond_t		cond;

	int				waiters;
};

/*���ڶԶ�д������⣺
*���writers = users, ��readers = users����ʾ����ͬʱs-lockҲ���Ի��x-lock,������free״̬
*���writers = users, readers != users, ����ϲ�����
*���readers = users, writes != users,��ʾrwlock���ų�read lock�����Ի��s-lock,���ܻ��x-lock, ���߳�����ռ��s-lock
*���readers != users, writes != users,��ʾ���߳�����waiting lock,����һ���߳�����ռ��x-lock
*������Щֵ�ĸı����CAS����������Ϻ�ȡ��
*/
typedef union
{
	uint64_t u;
	uint32_t us;			/*Ϊ��ԭ���ԵĶ�writers��readersͬʱ��ֵ,����x-lock��s-lock�Ĺ�ƽ��*/
	struct{
		uint16_t writers;
		uint16_t readers;
		uint16_t users;
		uint16_t pad;
	} s;
}wt_rwlock_t;

struct __wt_rwlock
{
	const char*		name;
	wt_rwlock_t		rwlock;
};

#define	SPINLOCK_GCC					0
#define	SPINLOCK_PTHREAD_MUTEX			1
#define	SPINLOCK_PTHREAD_MUTEX_ADAPTIVE	2
#define	SPINLOCK_PTHREAD_MUTEX_LOGGING	3
#define	SPINLOCK_MSVC					4

/*����spin lock������*/
typedef int  WT_SPINLOCK;
