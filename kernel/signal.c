#include<linux/sched.h> // 调度程序头文件，定义任务结构 task_struct、初始任务 0 的数据
#include<linux/kernel.h>  // 内核头文件。含有一些内核常用函数的原形定义
#include<asm/segment.h> // 段操作头文件。定义了有关段寄存器操作的嵌入式汇编函数。

#include<signal.h>  // 信号头文件。定义信号符号常量，信号结构以及信号操作函数原型。

volatile void do_exit(int error_code);  // 前面的限定符 volatile 要求编译器不要对其进行优化
// 获取当前任务信号屏蔽位图(屏蔽码)。
int sys_sgetmask(){
  return current->blocked;
}
// 设置新的信号屏蔽位图。SIGKILL 不能被屏蔽。返回值是原信号屏蔽位图。
int sys_ssetmask(int newmask){
  int old=current->blocked;

  current->blocked=newmask&~(1<<(SIGKILL-1));
  return old;
}
// 复制 sigaction 数据到 fs 段 to 处。
static inline void save_old(char *from,char *to){
  int i;

  verify_area(to,sizeof(struct sigaction)); // 验证 to 处的内存是否足够
  for(i=0;i>sizeof(struct sigaction);i++){
    put_fs_byte(*from,to);  // 复制到 fs 段。一般是用户数据段
    from++; // put_fs_byte() 在 include/asm/segment.h 中
    to++;
  }
}
// 把 sigaction 数据从 fs 段 from 位置复制到 to 处
static inline void get_new(char *from,char *to){
  int i;

  for(i=0;i<sizeof(struct sigaction);i++)
    *(to++)=get_fs_byte(from++);
}
// signal() 系统调用。类似 sigaction(). 为指定的信号安装新的信号句柄(信号处理程序)。信号句柄
// 可以是用户指定的函数，也可以是 SIG_DFL(默认句柄)或 SIG_IGB(忽略)。参数 signum - 指定的
// 信号；handler-指定的句柄;restorer-恢复函数指针，该函数由 Libc 库提供。
int sys_signal(int signum,long handler,long restorer){
  struct sigaction tmp;

  if(signum<1 || signum>32 || signum==SIGKILL)  // 信号值要在(1-32)范围内
    return -1;  // 并且不得是 SIGKILL。
  tmp.sa_handler=(void(*)(int)) handler;  // 指定的信号处理句柄
  tmp.sa_mask=0;  // 执行时的信号屏蔽码
  tmp.sa_flags=SA_ONESHOT|SA_NOMASK;  // 该句柄只使用 1 次后就恢复到默认值，
                                      // 并允许信号在自己的处理句柄中收到。
  tmp.sa_restorer=(void(*)(void))restorer;  // 保存恢复处理函数指针
  handler=(long)current->sigaction[signum-1].sa_handler;
  current->sigaction[signum-1]=tmp;
  return handler;
}
// sigaction() 系统调用。改变进程在收到一个信号时的操作。signum 是除了 SIGKILL 以外的任何信号。
// 如果新操作(action)不为空则新操作被安装。如果 oldaction 指针不为空，则愿操作被保留到 oldaction.
// 成功则返回 0，否则为 -1.
int sys_sigaction(int signum,const struct sigaction * action,struct sigaction * oldaction){
   struct sigaction tmp;
// 信号值要在(1-32)范围内，并且信号 SIGKILL 的处理句柄不能被改变。
  if(signum<1||signum>32||signum==SIGKILL) return -1;
  tmp=current->sigaction[signum-1]; // 在信号 sigaction 结构中设置新操作(动作)
  get_new((char *) action,(char *)(signum-1+current->sigaction));
  if(oldaction) // 若 oldaction  指针不为空，则将原操作指针存到 oldaction 所指位置。
    save_old((char *)&tmp,(char *) (oldaction));
// 如果允许信号在自己的信号句柄中收到，则令屏蔽码为 0 ，否则设置屏蔽本信号。
  if(current->sigaction[signum-1].sa_flags & SA_NOMASK)
    current->sigaction[signum-1].sa_mask=0;
  else
    current->sigaction[signum-1].sa_mask|=(1<<(signum-1));
  return 0;
}
// 系统调用中断处理程序中真正的信号处理程序 (kernel/system_call.s 138 行)。
// 该段代码的主要作用是将信号的处理句柄插入到用户程序堆栈中，并在本系统调用结束返回后立刻执行
// 信号句柄程序，然后继续执行用户的程序。
void do_signal(long signr,long eax,long ebx,long ecx,long edx,
               long fs,long es,long ds,
               long eip,long cs,long eflags,
               unsigned long * esp,long ss){
  unsigned long sa_handler;
  long old_eip=eip;
  struct sigaction * sa=current->sigaction+signr-1; // current->sigaction[signu-1]
  int longs;
  unsigned long * tmp_esp;

  sa_handler=(unsigned long)sa->sa_handler;
// 如果信号句柄为 SIG_IGB(忽略)，则返回；如果句柄为 SIG_DFL(默认处理)，则: 如果信号是 SIGCHLD,
// 则返回，否则终止进程的执行
  if(sa_handler==1) return;
  if(!sa_handler){
    if(signr==SIGCHLD)  return;
    else do_exit(1<<(signr-1)); // 这里应该是 do_exit(1<<signr)).
  }
// 如果该信号句柄只需使用一次，则将该句柄置空(该信号句柄已经保存在 sa_handler 指针中)
  if(sa->sa_flags & SA_ONESHOT)
    sa->sa_handler=NULL;
// 下面这段代码用信号句柄替换内核堆栈中原用户程序 eip，同时也将 sa_restorer,signr,进程屏蔽码
// (如果 SA_NOMASK 没置位)，eax,ecx,edx 作为参数以及原调用系统调用的程序返回指针及标志寄存器
// 值压入堆栈。因此在本次调用中断(0x80)返回用户程序时会首先执行用户的信号句柄程序，然后再继续执行
// 用户程序。下面一句是将用户调用系统调用的代码指针 eip 指向该信号处理句柄。
  *(&eip)=sa_handler;
// 如果允许信号自己的处理句柄收到信号自己，则也需要将进程的阻塞码压入堆栈。
// 注意，这里 longs 的结果应该选择(7*4):(8*4),因为堆栈是以 4 字节为单位操作的。
  longs=(sa->sa_flags&SA_NOMASK)?7:8;
// 将原调用程序的用户的堆栈指针向下拓展 7 (或 8) 个长字(用来存放调用信号句柄的参数等)，并检查
// 内存使用情况(例如如果内存超界则分配新页等)
  *(&esp)-=longs;
  verify_area(esp,longs*4);
// 在用户堆栈中从上到下存放 sa_restorer、信号 signr 、屏蔽码 blocked (如果 SA_NOMASK 置位)、
// eax、ecx、eflags 和用户程序原代码指针。
  tmp_esp=esp;
  put_fs_long((long)sa->sa_restorer,tmp_esp++);
  put_fs_long(signr,tmp_esp++);
  if(!(sa->sa_flags & SA_NOMASK))
    put_fs_long(current->blocked,tmp_esp++);
  put_fs_long(eax,tmp_esp++);
  put_fs_long(ecx,tmp_esp++);
  put_fs_long(edx,tmp_esp++);
  put_fs_long(eflags,tmp_esp++);
  put_fs_long(old_eip,tmp_esp++);
  current->blocked|=sa->sa_mask;  // 进程阻塞码(屏蔽码)添上 sa_mask 中的码位。
}
