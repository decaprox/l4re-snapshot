INTERFACE:

#include "context.h"
#include "icu_helper.h"
#include "types.h"

class Scheduler : public Icu_h<Scheduler>, public Irq_chip_soft
{
  FIASCO_DECLARE_KOBJ();
  typedef Icu_h<Scheduler> Icu;

public:
  enum Operation
  {
    Info       = 0,
    Run_thread = 1,
    Idle_time  = 2,
  };

  class Cpu_set
  {
  private:
    Mword _w;

  public:
    Mword offset() const { return (_w & 0x00ffffff) & (~0 << granularity()); }
    Mword granularity() const { return (_w >> 24) & (MWORD_BITS-1) ; }
    bool contains(unsigned cpu, Mword map) const
    {
      if (offset() > cpu)
	return false;

      cpu -= offset();
      cpu >>= granularity();
      if (cpu >= MWORD_BITS)
	return false;

      return map & (1UL << cpu);
    }
  };

  static Scheduler scheduler;
private:
  Irq_base *_irq;
};

// ----------------------------------------------------------------------------
IMPLEMENTATION:

#include "thread_object.h"
#include "l4_buf_iter.h"
#include "entry_frame.h"

FIASCO_DEFINE_KOBJ(Scheduler);

Scheduler Scheduler::scheduler;

PUBLIC void
Scheduler::operator delete (void *)
{
  printf("WARNING: tried to delete kernel scheduler object.\n"
         "         The system is now useless\n");
}

PUBLIC inline
Scheduler::Scheduler() : _irq(0)
{
  initial_kobjects.register_obj(this, 7);
}

PRIVATE static
Mword
Scheduler::first_online(Cpu_set const *cpus, Mword bm)
{
  unsigned cpu = cpus->offset();

  for (;;)
    {
      unsigned b = (cpu - cpus->offset()) >> cpus->granularity();
      if (cpu >= Config::Max_num_cpus || b >= MWORD_BITS)
	return ~0UL;

      if (!(bm & (1UL << b)))
	{
	  cpu += 1UL << cpus->granularity();
	  continue;
	}

      if (Cpu::online(cpu))
	return cpu;

      ++cpu;
    }
}

PRIVATE
L4_msg_tag
Scheduler::sys_run(unsigned char /*rights*/, Syscall_frame *f, Utcb const *utcb)
{
  L4_msg_tag const tag = f->tag();
  unsigned const curr_cpu = current_cpu();

  Obj_space *s = current()->space();
  assert(s);

  L4_snd_item_iter snd_items(utcb, tag.words());

  if (EXPECT_FALSE(tag.words() < 5))
    return commit_result(-L4_err::EInval);

  if (EXPECT_FALSE(!tag.items() || !snd_items.next()))
    return commit_result(-L4_err::EInval);

  L4_fpage _thread(snd_items.get()->d);
  if (EXPECT_FALSE(!_thread.is_objpage()))
    return commit_result(-L4_err::EInval);

  Thread *thread = Kobject::dcast<Thread_object*>(s->lookup_local(_thread.obj_index()));
  if (!thread)
    return commit_result(-L4_err::EInval);

  Cpu_set const *cpus = reinterpret_cast<Cpu_set const *>(&utcb->values[1]);

  Thread::Migration_info info;

  unsigned t_cpu = thread->cpu();
  if (cpus->contains(t_cpu, utcb->values[2]))
    info.cpu = t_cpu;
  else if (cpus->contains(curr_cpu, utcb->values[2]))
    info.cpu = curr_cpu;
  else
    info.cpu = first_online(cpus, utcb->values[2]);
#if 0
  if (info.cpu == Invalid_cpu)
    return commit_result(-L4_err::EInval);
#endif
  info.prio = utcb->values[3];
  info.quantum = utcb->values[4];
#if 0
  printf("CPU[%lx]: current_cpu=%u run(thread=%lx, prio=%ld, quantum=%ld, cpu=%ld (%lx,%u,%u)\n",
         dbg_id(), curr_cpu, thread->dbg_id(), info.prio, info.quantum, info.cpu, utcb->values[2], cpus->offset(), cpus->granularity());
#endif

  if (info.prio > 255)
    info.prio = 255;
  if (!info.quantum)
    info.quantum = Config::Default_time_slice;


  thread->migrate(info);

  return commit_result(0);
}

PRIVATE
L4_msg_tag
Scheduler::sys_idle_time(unsigned char,
                         Syscall_frame *f, Utcb *utcb)
{
  if (f->tag().words() < 3)
    return commit_result(-L4_err::EInval);

  Cpu_set const *cpus = reinterpret_cast<Cpu_set const *>(&utcb->values[1]);
  Mword const cpu = first_online(cpus, utcb->values[2]);
  if (cpu == ~0UL)
    return commit_result(-L4_err::EInval);

  reinterpret_cast<Utcb::Time_val *>(utcb->values)->t
    = Context::kernel_context(cpu)->consumed_time();

  return commit_result(0, Utcb::Time_val::Words);
}

PRIVATE
L4_msg_tag
Scheduler::sys_info(unsigned char, Syscall_frame *f,
                      Utcb const *iutcb, Utcb *outcb)
{
  if (f->tag().words() < 2)
    return commit_result(-L4_err::EInval);

  Cpu_set const *s = reinterpret_cast<Cpu_set const*>(&iutcb->values[1]);
  Mword rm = 0;
  Mword max = Config::Max_num_cpus;
  Mword const offset = s->offset() << s->granularity();
  if (offset >= max)
    return commit_result(-L4_err::EInval);

  if (max > offset + ((Mword)MWORD_BITS << s->granularity()))
    max = offset + ((Mword)MWORD_BITS << s->granularity());

  for (Mword i = 0; i < max - offset; ++i)
    if (Cpu::online(i + offset))
      rm |= (1 << (i >> s->granularity()));

  outcb->values[0] = rm;
  outcb->values[1] = Config::Max_num_cpus;
  return commit_result(0, 2);
}

PUBLIC inline
Irq_base *
Scheduler::icu_get_irq(unsigned irqnum)
{
  if (irqnum > 0)
    return 0;

  return _irq;
}


PUBLIC inline
void
Scheduler::icu_get_info(Mword *features, Mword *num_irqs, Mword *num_msis)
{
  *features = 0; // supported features (only normal irqs)
  *num_irqs = 1;
  *num_msis = 0;
}

PUBLIC
L4_msg_tag
Scheduler::icu_bind_irq(Irq *irq_o, unsigned irqnum)
{
  if (irqnum > 0)
    return commit_result(-L4_err::EInval);

  if (_irq)
    _irq->unbind();

  Irq_chip_soft::bind(irq_o, irqnum);
  _irq = irq_o;
  return commit_result(0);
}

PUBLIC inline
void
Scheduler::trigger_hotplug_event()
{
  if (_irq)
    _irq->hit(0);
}

PUBLIC
L4_msg_tag
Scheduler::kinvoke(L4_obj_ref ref, Mword rights, Syscall_frame *f,
                   Utcb const *iutcb, Utcb *outcb)
{
  switch (f->tag().proto())
    {
    case L4_msg_tag::Label_irq:
      return Icu::icu_invoke(ref, rights, f, iutcb,outcb);
    case L4_msg_tag::Label_scheduler:
      break;
    default:
      return commit_result(-L4_err::EBadproto);
    }

  switch (iutcb->values[0])
    {
    case Info:       return sys_info(rights, f, iutcb, outcb);
    case Run_thread: return sys_run(rights, f, iutcb);
    case Idle_time:  return sys_idle_time(rights, f, outcb);
    default:         return commit_result(-L4_err::ENosys);
    }
}
