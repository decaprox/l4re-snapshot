IMPLEMENTATION:

#include <cstdio>
#include <cstring>

#include "config.h"
#include "globals.h"
#include "ipc_timeout.h"
#include "jdb.h"
#include "jdb_kobject.h"
#include "jdb_kobject_names.h"
#include "jdb_module.h"
#include "jdb_screen.h"
#include "kernel_console.h"
#include "keycodes.h"
#include "kmem.h"
#include "simpleio.h"
#include "static_init.h"
#include "timeout.h"
#include "timeslice_timeout.h"
#include "thread.h"

class Jdb_list_timeouts : public Jdb_module
{
public:
  Jdb_list_timeouts() FIASCO_INIT;
private:
  enum
  {
    Timeout_ipc       = 1,
    Timeout_deadline  = 2,
    Timeout_timeslice = 3
  };
};


namespace {

class Timeout_iter
{
private:
  typedef Timeout::To_list::Const_iterator To_iter;

  int skip_empty(To_iter *c, int i)
  {
    while (*c == _q->first(i).end() && i < (int)_q->queues())
      {
        ++i;
        *c = _q->first(i).begin();
      }
    return i;
  }

public:
  explicit Timeout_iter(Timeout_q *t)
  : _q(t), _i(0)
  { rewind(); }

  explicit Timeout_iter(Timeout_q *t, bool)
  : _q(t), _c(t->first(0).end()), _i(0)
  {}

  void rewind()
  {
    _i = 0;
    _c = _q->first(0).begin();
    _i = skip_empty(&_c, _i);
  }

  Timeout_iter const &operator ++ ()
  {
    if (_c == _q->first(_i).end())
      return *this;

    ++_c;
    _i = skip_empty(&_c, _i);

    return *this;
  }

  Timeout *operator * () const { return *_c; }

  bool operator == (Timeout_iter const &o) const
  { return _c == o._c; }

  bool operator != (Timeout_iter const &o) const
  { return _c != o._c; }

private:
  Timeout_q *_q;
  To_iter _c;
  int _i;
};

template< typename FWD_ITER >
class Rnd_container
{
public:
  explicit Rnd_container(FWD_ITER const &b, FWD_ITER const &e)
  : _b(b), _e(e), _cnt(0)
  {
    for (FWD_ITER i = b; i != e; ++i)
      ++_cnt;
  }

  class Iterator : public FWD_ITER
  {
  public:
    Iterator() {}

    Iterator const &operator += (int offs)
    {
      if (offs < 0)
        return operator -= (-offs);

      if (offs == 0)
        return *this;

      for (; *this != _c->_e && offs > 0; --offs, ++_p)
        this->operator ++ ();

      return *this;
    }

    Iterator const &operator -= (int offs)
    {
      if (offs < 0)
        return this->operator += (-offs);

      if (offs == 0)
        return *this;

      int p = 0;

      if (_p > (unsigned)offs)
        p = _p - offs;

      FWD_ITER i = _c->_b;

      for (int z = 0; z < p && i != _c->_e; ++i, ++z)
        ;

      *this = Iterator(_c, p, i);
      return *this;
    }

    Iterator const &operator -- ()
    { return this->operator -= (1); }

    int pos() const { return _p; }

  private:
    friend class Rnd_container;

    Iterator(Rnd_container *c, unsigned long pos, FWD_ITER const &i)
    : FWD_ITER(i), _c(c), _p(pos)
    {}

    Rnd_container *_c;
    unsigned long _p;
  };

public:
  Iterator begin() { return Iterator(this, 0, _b); }
  Iterator end() { return Iterator(this, _cnt, _e); }
  unsigned long size() const { return _cnt; }
  Iterator at(unsigned long pos)
  {
    if (pos >= _cnt)
      return _e;

    Iterator i = _b;
    i += pos;
    return i;
  }

private:
  FWD_ITER _b, _e;
  unsigned long _cnt;
};

}


// available from the jdb_tcb module
extern int jdb_show_tcb(Thread *thread, int level) __attribute__((weak));

// use implicit knowledge to determine the type of a timeout because we
// cannot use dynamic_cast (we compile with -fno-rtti)
static
int
Jdb_list_timeouts::get_type(Timeout *t)
{
  Address addr = (Address)t;

  if (t == timeslice_timeout.cpu(0))
    // there is only one global timeslice timeout
    return Timeout_timeslice;

  if ((addr % Context::Size) >= sizeof(Thread))
    // IPC timeouts are located at the kernel stack
    return Timeout_ipc;

  // unknown
  return 0;
}

static
Thread*
Jdb_list_timeouts::get_owner(Timeout *t)
{
  switch (get_type(t))
    {
      case Timeout_ipc:
        return static_cast<Thread*>(context_of(t));
      case Timeout_deadline:
        return static_cast<Thread*>(context_of(t));
      case Timeout_timeslice:
        return static_cast<Thread*>(Context::kernel_context(0));
        // XXX: current_sched does not work from the debugger
        if (Context::current_sched())
          return static_cast<Thread*>(Context::current_sched()->context());
      default:
        return 0;
    }
}

static
void
Jdb_list_timeouts::show_header()
{
  Jdb::cursor();
  printf("%s  type           timeout    owner       name\033[m\033[K\n",
         Jdb::esc_emph);
}

static
void
Jdb_list_timeouts::list_timeouts_show_timeout(Timeout *t)
{
  char const *type;
  char ownerstr[32] = "";
  Thread *owner;
  Signed64 timeout = t->get_timeout(Kip::k()->clock);

  Kconsole::console()->getchar_chance();

  switch (get_type(t))
    {
    case Timeout_ipc:
      type  = "ipc";
      owner = get_owner(t);
      snprintf(ownerstr, sizeof(ownerstr), "  %p", owner);
      break;
    case Timeout_deadline:
      type  = "deadline";
      owner = get_owner(t);
      snprintf(ownerstr, sizeof(ownerstr), "  %p", owner);
      break;
    case Timeout_timeslice:
      type  = "timeslice";
      owner = get_owner(t);
      if (owner)
        snprintf(ownerstr, sizeof(ownerstr), "  %p", owner);
      else
       strcpy (ownerstr, "destruct");
      break;
    default:
      snprintf(ownerstr, sizeof(ownerstr), L4_PTR_FMT, (Address)t);
      type  = "???";
      owner = 0;
      break;
    }

  printf("  %-10s   ", type);
  if (timeout < 0)
    putstr("   over     ");
  else
    {
      char time_str[12];
      Jdb::write_ll_ns(timeout * 1000, time_str,
                       11 < sizeof(time_str) - 1 ? 11 : sizeof(time_str) - 1,
                       false);
      putstr(time_str);
    }

  Jdb_kobject_name *nx = 0;

  if (owner)
    nx = Jdb_kobject_extension::find_extension<Jdb_kobject_name>(owner);

  printf(" %s  %s\033[K\n", ownerstr, nx ? nx->name() : "");
}

IMPLEMENT
Jdb_list_timeouts::Jdb_list_timeouts()
  : Jdb_module("INFO")
{}

static
void
Jdb_list_timeouts::list()
{
  unsigned y, y_max;

  typedef Rnd_container<Timeout_iter> Cont;
  typedef Cont::Iterator Iter;

  Cont to_cont(Timeout_iter(&Timeout_q::timeout_queue.cpu(0)), Timeout_iter(&Timeout_q::timeout_queue.cpu(0), true));
  Iter first = to_cont.begin();
  Iter current = first;
  Iter end = to_cont.end();
  --end;

  Jdb::clear_screen();
  show_header();
  for (;;)
    {
      y = current.pos();

      for (bool resync=false; !resync; )
        {
          Jdb::cursor(2, 1);
          y_max = 0;
          for (Iter i = current; i != to_cont.end(); ++i, ++y_max)
            list_timeouts_show_timeout(*i);

          for (unsigned i=y_max; i<Jdb_screen::height()-3; ++i)
            putstr("\033[K\n");

          Jdb::printf_statline("timouts", "<CR>=select owner", "_");

          for (bool redraw=false; !redraw; )
            {
              Jdb::cursor(y+2, 1);
              switch (int c=Jdb_core::getchar())
                {
                case KEY_CURSOR_UP:
                  if (y > 0)
                    y--;
                  else
                    {
                      Iter i = current;
                      --current;
                      redraw = i != current;
                    }
                  break;
                case KEY_CURSOR_DOWN:
                  if (y < y_max)
                    y++;
                  else
                    {
                      Iter i = current;
                      ++current;

                      if (current == to_cont.end())
                        current = i;
                      redraw = i != current;
                    }
                  break;
                case KEY_PAGE_UP:
                    {
                      Iter i = current;
                      current -= Jdb_screen::height()-3;
                      redraw =  i != current;
                      if (!redraw)
                        y = 0;
                    }
                  break;
                case KEY_PAGE_DOWN:
                    {
                      Iter i = current;
                      current += Jdb_screen::height()-3;
                      if (current == to_cont.end())
                        current = end;
                      redraw =  i != current;
                      if (!redraw)
                        y = y_max;
                    }
                  break;
                case KEY_CURSOR_HOME:
                  redraw = current != first;
                  current = first;
                  y = 0;
                  break;
                case KEY_CURSOR_END:
                  redraw = current != end;
                  current = end;
                  y = y_max;
                  break;
                case KEY_RETURN:
                  if (jdb_show_tcb != 0)
                    {
                      Thread *owner;
                      Iter i = current;
                      i += y;
                      if (i != to_cont.end() && (owner = get_owner(*i)))
                        {
                          if (!jdb_show_tcb(owner, 1))
                            return;
                          show_header();
                          redraw = 1;
                        }
                    }
                  break;
                case KEY_ESC:
                  Jdb::abort_command();
                  return;
                default:
                  if (Jdb::is_toplevel_cmd(c))
                    return;
                }
            }
        }
    }
}

PUBLIC
Jdb_module::Action_code
Jdb_list_timeouts::action(int cmd, void *&, char const *&, int &)
{
  if (cmd == 0)
    list();

  return NOTHING;
}

PUBLIC
Jdb_module::Cmd const *
Jdb_list_timeouts::cmds() const
{
  static Cmd cs[] =
    {
        { 0, "lt", "timeouts", "", "lt\tshow enqueued timeouts", 0 },
    };

  return cs;
}

PUBLIC
int
Jdb_list_timeouts::num_cmds() const
{
  return 1;
}

static Jdb_list_timeouts jdb_list_timeouts INIT_PRIORITY(JDB_MODULE_INIT_PRIO);
