#include "app_loading"
#include "locking.h"

#define MSG() DEBUGf(Romain::Log::Loader)

Romain::App_model::Const_dataspace Romain::App_model::open_file(char const *name)
{
	MSG() << name;

	int err = open(name, O_RDONLY);
	MSG() << "fopen: " << err;

	cxx::Ref_ptr<L4Re::Vfs::File> fp = L4Re::Vfs::vfs_ops->get_file(err);
	MSG() << "file ptr @ " << fp;

	Romain::App_model::Dataspace ds_cap = fp->data_space();
	MSG() << "ds @ 0x" << std::hex << ds_cap.cap();

	return ds_cap;
}

l4_addr_t Romain::App_model::local_attach_ds(Romain::App_model::Const_dataspace ds,
                                             unsigned long size,
                                             unsigned long offset) const
{
	Romain::Rm_guard r(rm(), 0); // we always init stuff for instance 0,
	// rest is paged in lazily
	// XXX: duplicate, remove
#if 0
	MSG() << std::hex << ds.cap() << " "
	      << size << " "
	      << offset << " "
	      << l4_round_page(offset);
#endif
	unsigned flags = L4Re::Rm::Search_addr;

	/* This function is called during startup for attaching the
	 * binary DS read-only to read ELF info. However, in this case
	 * we don't have any flags indicating this and writing the DS will
	 * fail. Therefore, this hacky check adds an RO flag so that the
	 * memory manager performs the right thing.
	 */
	if (ds == binary()) {
		flags |= L4Re::Rm::Read_only;
	}
	Romain::Region_handler handler(ds, L4_INVALID_CAP, offset, 0,
	                            Romain::Region(0, 0));
	l4_addr_t ret = (l4_addr_t)rm()->attach_locally((void*)0xdeadbeef,
	                                                size, &handler, flags);
	return ret;
}


void Romain::App_model::local_detach_ds(l4_addr_t addr, unsigned long /*size*/) const
{
#if 0
	MSG() << (void*)addr;
#endif

	long r = L4Re::Env::env()->rm()->detach(addr, 0);
	_check(r != 0, "detach error");
}


int Romain::App_model::prog_reserve_area(l4_addr_t *start, unsigned long size,
                                      unsigned flags, unsigned char align)
{
	MSG() << (void*)start << " "
	      << size << " "
	      << flags << " "
	      << align;
	_check(1, "unimplemented");
	return -1;
}


Romain::App_model::Dataspace Romain::App_model::alloc_app_stack()
{
#if 0
	MSG() << __func__ << " " << _stack.stack_size() << " "
	      << std::hex << (void*)_stack.ptr();
#endif
	Romain::App_model::Dataspace ds = this->alloc_ds(_stack.stack_size());
	char *addr = reinterpret_cast<char*>(this->local_attach_ds(ds, _stack.stack_size(), 0));
	MSG() << "attached app stack: " << (void*)addr;
	_local_stack_address = l4_addr_t(addr);

	//_stack.ptr(addr);
	_stack.set_local_top(addr + _stack.stack_size());
	MSG() << "stack local : " << std::hex << (void*)_stack.ptr();
	MSG() << "stack remote: " << std::hex << (void*)_stack.relocate(_stack.ptr());

	/*
	 * Place UTCB area pointer at the very top of the stack.
	 * This way we know exactly what to put into the FS segment
	 * descriptor.
	 */
	_stack.push(prog_info()->utcbs_start);

	return ds;
}


void Romain::App_model::prog_attach_kip()
{
	MSG() << "ATTACHING KIP: " << (void*)this->prog_info()->kip;
	this->prog_attach_ds(this->prog_info()->kip, L4_PAGESIZE,
	                     this->local_kip_ds(), 0,
	                     L4Re::Rm::Read_only, "attaching KIP segment",
	                     (l4_addr_t)l4re_kip());
	//enter_kdebug("kip");
}


void Romain::App_model::prog_attach_ds(l4_addr_t addr, unsigned long size,
                                    Romain::App_model::Const_dataspace ds, unsigned long offset,
                                    unsigned flags, char const *what, l4_addr_t local_start)
{
	(void)what;
	Romain::Rm_guard r(rm(), 0); // we always init stuff for instance 0,
	                          // rest is paged in lazily
#if 0
	MSG() << std::hex << (void*)addr << " "
	      << size << " "
	      << ds.cap() << " "
	      << offset << " "
	      << flags << " "
	      << local_start << " "
	      << what;
#endif

	/*
	 * This is where we remember the initial segments.
	 */
	void* target = rm()->attach((void*)addr, size,
	                            Romain::Region_handler(
	                                ds, L4_INVALID_CAP, offset, flags,
	                                Romain::Region(local_start, local_start + size - 1)),
	                                flags);
#if 0
	MSG() << std::hex << (void*)target;
#endif

	_check(((target == 0) || (target == (void*)~0UL)), "error attaching segment");
}


L4Re::Env*
Romain::App_model::add_env()
{
	_stack.push(l4re_env_cap_entry_t());
	push_initial_caps();
	L4Re::Env::Cap_entry *remote_cap_ptr =
		reinterpret_cast<L4Re::Env::Cap_entry*>(_stack.relocate(_stack.ptr()));
	MSG() << "remote cap entries @ " << (void*)remote_cap_ptr;

	L4Re::Env *env = _stack.push(L4Re::Env());
	memcpy(env, L4Re::Env::env(), sizeof(L4Re::Env));
	MSG() << "my first cap " << std::hex << env->first_free_cap();
	env->initial_caps(remote_cap_ptr);
	env->first_free_cap(0x1000);

	return env;
}


Romain::App_model::Dataspace Romain::App_model::alloc_ds(unsigned long size) const
{
	Dataspace ds = L4Re::Util::cap_alloc.alloc<L4Re::Dataspace>();
	_check(!ds.is_valid(), "ds cap allocation failed");

	long r = L4Re::Env::env()->mem_alloc()->alloc(size, ds);
	_check(r != 0, "dataspace allocation failed");

	return ds;
}

void Romain::App_model::copy_ds(Romain::App_model::Dataspace dst, unsigned long dst_offs,
                                Const_dataspace src, unsigned long src_offs,
                                unsigned long size)
{
#if 0
	MSG() << std::hex << dst.cap() << " "
	      << dst_offs << " "
	      << src.cap() << " "
	      << src_offs << " "
	      << size;
#endif
	dst->copy_in(dst_offs, src, src_offs, size);
#if 0
	MSG() << "...done";
#endif
}


void Romain::App_model::prog_attach_stack(Romain::App_model::Dataspace app_stack)
{
#if 0
	MSG() << __func__ << std::hex
	      << " " << app_stack.cap()
	      << " " << (void*)_stack.target_addr()
	      << " " << (void*)_stack.target_top();
#endif
	this->prog_attach_ds(this->_stack.target_addr(),
	                     this->_stack.stack_size(),
	                     app_stack,
	                     0, 0, "stack segment", _local_stack_address);
	prog_info()->stack_addr = _stack.target_addr() + _stack.stack_size();
#if 0
	MSG() << std::hex << (void*)_stack.ptr() << " <-> "
	      << (void*)_stack.relocate(_stack.ptr());
#endif
}


void Romain::App_model::prog_reserve_utcb_area()
{
	_utcb_area = this->alloc_ds(1 << prog_info()->utcbs_log2size);
#if 0
	MSG() << __func__ << " " << std::hex
	      << (void*)this->prog_info()->utcbs_start;
#endif
	this->prog_attach_ds(this->prog_info()->utcbs_start,
	                     1 << this->prog_info()->utcbs_log2size,
	                     _utcb_area, 0, 0, "utcb area",
	                     0);
}


void const *Romain::App_model::generate_l4aux(char const *name)
{
	this->_stack.push(l4_umword_t(this->prog_info()->ldr_flags));
	this->_stack.push(l4_umword_t(this->prog_info()->l4re_dbg));
	this->_stack.push(l4_umword_t(local_kip_ds().cap()));
	return this->_stack.push_local_ptr(name);
}
