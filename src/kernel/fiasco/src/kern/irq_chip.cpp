INTERFACE:

#include "types.h"

class Irq_base;
class Irq_chip;

class Upstream_irq
{
private:
  Irq_chip *const _c;
  Mword const _p;
  Upstream_irq const *const _prev;

};

/**
 * Abstraction for an IRQ controller chip.
 */
class Irq_chip
{
public:
  virtual void mask(Mword pin) = 0;
  virtual void unmask(Mword pin) = 0;
  virtual void ack(Mword pin) = 0;
  virtual void mask_and_ack(Mword pin) = 0;

  /**
   * Set the trigger mode and polarity.
   */
  virtual unsigned set_mode(Mword pin, unsigned) = 0;

  /**
   * Set the target CPU.
   * \param pin the pin to configure
   * \param cpu the logical CPU number.
   */
  virtual void set_cpu(Mword pin, unsigned cpu) = 0;
  virtual void unbind(Irq_base *irq);
  virtual ~Irq_chip() = 0;
};

/**
 * Artificial IRQ chip, used for SW IRQs.
 */
class Irq_chip_soft : public Irq_chip
{
public:
  void mask(Mword) {}
  void unmask(Mword) {}
  void mask_and_ack(Mword) {}
  void ack(Mword) {}

  void set_cpu(Mword, unsigned) {}
  unsigned set_mode(Mword, unsigned mode) { return mode; }

  char const *chip_type() const { return "Soft"; }

  static Irq_chip_soft sw_chip;
};

/**
 * Abstract IRQ controller chip that is visble as part of the
 * Icu to the user.
 */
class Irq_chip_icu : public Irq_chip
{
public:
  virtual bool reserve(Mword pin) = 0;
  virtual bool alloc(Irq_base *irq, Mword pin) = 0;
  virtual Irq_base *irq(Mword pin) const = 0;
  virtual unsigned nr_irqs() const = 0;
  virtual ~Irq_chip_icu() = 0;
};


class Kobject_iface;


/**
 * Base class for all kinds of IRQ consuming objects.
 */
class Irq_base
{
  friend class Irq_chip;

public:

  typedef void (*Hit_func)(Irq_base *, Upstream_irq const *);
  enum Flags
  {
    F_enabled = 1,
  };

  enum Mode
  {
    Set_irq_mode  = 0x1,
    Trigger_edge  = 0x0,
    Trigger_level = 0x2,
    Trigger_mask  = 0x2,

    Polarity_high = 0x0,
    Polarity_low  = 0x4,
    Polarity_both = 0x8,
    Polarity_mask = 0xc,
  };

  Irq_base() : _flags(Trigger_level), _next(0)
  {
    Irq_chip_soft::sw_chip.bind(this, 0, true);
    mask();
  }

  void hit(Upstream_irq const *ui) { hit_func(this, ui); }

  Mword pin() const { return _pin; }
  Irq_chip *chip() const { return _chip; }

  void mask() { if (!__mask()) _chip->mask(_pin); }
  void mask_and_ack() { if (!__mask()) _chip->mask_and_ack(_pin); }
  void unmask() { if (__unmask()) _chip->unmask(_pin); }
  void ack() { _chip->ack(_pin); }


  void set_mode(unsigned m)
  {
    unsigned mode = _chip->set_mode(_pin, m);
    _flags = (_flags & ~0xe) | (mode & 0xe);
    switch_mode(mode);
  }

  void set_cpu(unsigned cpu) { _chip->set_cpu(_pin, cpu); }

  unsigned get_mode() const
  { return _flags & 0xe; }

  bool masked() const { return !(_flags & F_enabled); }
  Mword flags() const { return _flags; }

  void unbind() { _chip->unbind(this); }

  bool __mask() { bool o = masked(); _flags &= ~F_enabled; return o; }
  bool __unmask() { bool o = masked(); _flags |= F_enabled; return o; }

  void set_hit(Hit_func f) { hit_func = f; }
  virtual void switch_mode(unsigned mode) = 0;
  virtual ~Irq_base() = 0;

protected:
  Hit_func hit_func;

  Irq_chip *_chip;
  Mword _pin;
  unsigned _flags;

  template<typename T>
  static void handler_wrapper(Irq_base *irq, Upstream_irq const *ui)
  { nonull_static_cast<T*>(irq)->handle(ui); }

public:
  Irq_base *_next;

  static Irq_base *(*dcast)(Kobject_iface *);
};




//----------------------------------------------------------------------------
INTERFACE [debug]:

EXTENSION class Irq_chip
{
public:
  virtual char const *chip_type() const = 0;
};


//--------------------------------------------------------------------------
IMPLEMENTATION:

#include "types.h"
#include "cpu_lock.h"
#include "lock_guard.h"
#include "static_init.h"

Irq_chip_soft Irq_chip_soft::sw_chip INIT_PRIORITY(EARLY_INIT_PRIO);
Irq_base *(*Irq_base::dcast)(Kobject_iface *);

IMPLEMENT inline Irq_chip::~Irq_chip() {}
IMPLEMENT inline Irq_chip_icu::~Irq_chip_icu() {}
IMPLEMENT inline Irq_base::~Irq_base() {}

PUBLIC inline explicit
Upstream_irq::Upstream_irq(Irq_base const *b, Upstream_irq const *prev)
: _c(b->chip()), _p(b->pin()), _prev(prev)
{}

PUBLIC inline
void
Upstream_irq::ack() const
{
  for (Upstream_irq const *c = this; c; c = c->_prev)
    c->_c->ack(c->_p);
}


PUBLIC inline
void
Irq_chip::bind(Irq_base *irq, Mword pin, bool ctor = false)
{
  irq->_pin = pin;
  irq->_chip = this;

  if (ctor)
    return;

  irq->set_mode(irq->get_mode());
  if (irq->masked())
    mask(pin);
  else
    unmask(pin);
}

IMPLEMENT inline
void
Irq_chip::unbind(Irq_base *irq)
{
  Irq_chip_soft::sw_chip.bind(irq, 0, true);
}


/**
 * \param CHIP must be the dynamic type of the object.
 */
PUBLIC inline
template<typename CHIP>
void
Irq_chip::handle_irq(Mword pin, Upstream_irq const *ui)
{
  // call the irq function of the chip avoiding the
  // virtual function call overhead.
  Irq_base *irq = nonull_static_cast<CHIP*>(this)->CHIP::irq(pin);
  irq->log();
  irq->hit(ui);
}

PUBLIC inline
template<typename CHIP>
void
Irq_chip::handle_multi_pending(Upstream_irq const *ui)
{
  while (Mword pend = nonull_static_cast<CHIP*>(this)->CHIP::pending())
    {
      for (unsigned i = 0; i < sizeof(Mword)*8; ++i, pend >>= 1)
	if (pend & 1)
	  {
	    handle_irq<CHIP>(i, ui);
	    break; // read the pending ints again
	  }
    }
}


PUBLIC inline NEEDS["lock_guard.h", "cpu_lock.h"]
void
Irq_base::destroy()
{
  Lock_guard<Cpu_lock> g(&cpu_lock);
  unbind();
}


// --------------------------------------------------------------------------
IMPLEMENTATION [!debug]:

PUBLIC inline void Irq_base::log() {}

//-----------------------------------------------------------------------------
INTERFACE [debug]:

#include "tb_entry.h"

EXTENSION class Irq_base
{
public:
  struct Irq_log
  {
    Irq_base *obj;
    Irq_chip *chip;
    Mword pin;
  };

  static unsigned irq_log_fmt(Tb_entry *, int, char *)
  asm ("__irq_log_fmt");
};


// --------------------------------------------------------------------------
IMPLEMENTATION [debug]:

#include <cstdio>

#include "logdefs.h"
#include "kobject_dbg.h"

IMPLEMENT
unsigned
Irq_base::irq_log_fmt(Tb_entry *e, int maxlen, char *buf)
{
  Irq_log *l = e->payload<Irq_log>();
  Kobject_dbg::Const_iterator irq = Kobject_dbg::pointer_to_obj(l->obj);

  if (irq != Kobject_dbg::end())
    return snprintf(buf, maxlen, "0x%lx/%lu @ chip %s(%p) D:%lx",
                    l->pin, l->pin, l->chip->chip_type(), l->chip,
                    irq->dbg_id());
  else
    return snprintf(buf, maxlen, "0x%lx/%lu @ chip %s(%p) irq=%p",
                    l->pin, l->pin, l->chip->chip_type(), l->chip,
                    l->obj);
}

PUBLIC inline NEEDS["logdefs.h"]
void
Irq_base::log()
{
  Context *c = current();
  LOG_TRACE("IRQ-Object triggers", "irq", c, __irq_log_fmt,
      Irq_base::Irq_log *l = tbe->payload<Irq_base::Irq_log>();
      l->obj = this;
      l->chip = chip();
      l->pin = pin();
  );
}

