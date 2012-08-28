#pragma once

#include "../fault_observers"
#include "debugging.h" // Breakpoint
#if 0
#include "../gdb_stub/gdbserver"
#include "../gdb_stub/connection"
#endif
#include "../configuration"

namespace Romain
{
	/*
	 * Debug observer - can set breakpoint on a single instruction and
	 * single-steps the application after hitting the BP.
	 */
	class SimpleDebugObserver : public Observer
	{
		private:
			Breakpoint *_bp;
			l4_umword_t _int1_seen;

			l4_addr_t determine_address()
			{
				l4_addr_t addr = ConfigIntValue("simpledbg:singlestep");
				DEBUG() << "Single step target: 0x"
				        << std::hex << addr;

				return addr;
			}

			DECLARE_OBSERVER("simple dbg");
			SimpleDebugObserver();
	};


	/*
	 * INT3 trap observer (aka JDB emulator)
	 */
	class TrapObserver : public Observer
	{
		DECLARE_OBSERVER("trap");
		static char const * const jdb_out_prefix;
		static char const * const jdb_out_suffix;
	};


	/*
	 * Observer that prints VCPU content on every fault.
	 */
	class PrintVCPUStateObserver : public Observer
	{
		DECLARE_OBSERVER("print vcpu state");
	};


	/*
	 * Observer for handling page faults.
	 */
	class PageFaultObserver : public Observer
	{
		private:
		bool _readonly;

		DECLARE_OBSERVER("pf");
		PageFaultObserver();

		bool always_readonly() const { return _readonly; }
	};


	/*
	 * System call handling
	 */
	class SyscallObserver : public Observer
	{
		DECLARE_OBSERVER("syscalls");

		protected:

		/*
		 * Store UTCB from orig to backup.
		 */
		void store_utcb(char *orig, char *backup) { memcpy(backup, orig, L4_UTCB_OFFSET); }

		/*******************************************
		 * Perform system call redirection.
		 *******************************************/
		void do_proxy_syscall(Romain::App_instance*, Romain::App_thread*, Romain::App_model*);

		/*******************************************
		 * Memory-management-related system calls.
		 *******************************************/
		void handle_rm(Romain::App_instance*, Romain::App_thread*, Romain::App_model*);

		/*******************************************
		 * Thread-related system calls
		 *******************************************/
		void handle_thread(Romain::App_instance*, Romain::App_thread*, Romain::App_model*);
		void handle_task(Romain::App_instance*, Romain::App_thread*, Romain::App_model*);
		/*
		 * gdt setup system calls
		 */
		void handle_thread_gdt(Romain::App_thread*, l4_utcb_t *u);
	};


	/*
	 * Limit for the amount of traps handled before exiting
	 *
	 * Debugging feature: for unit tests we want to run an application for a
	 * dedicated amount of system calls until terminating the run, so that the
	 * output becomes reproducible.
	 */
	class TrapLimitObserver : public Observer
	{
		public:
		static TrapLimitObserver *Create();
	};


	/*
	 * Observer monitoring accesses to the clock field in the KIP.
	 *
	 * The KIP is mostly read-only and we may therefore get along with a single copy
	 * that is mapped read-only to all clients. However, the KIP contains a clock field
	 * that is updated on every context switch if the kernel has been compiled with
	 * FINE_GRAIN_CPU_TIME. This field is used by gettimeofday(). In order to let all
	 * replicas see the same time when they use this function, we need to instrument
	 * accesses to the clock field.
	 */
	class KIPTimeObserver : public Observer
	{
		public:
		static KIPTimeObserver *Create();
	};


	/*
	 * Software-Implemented Fault Injection
	 */
	class SWIFIObserver : public Observer
	{
		public:
		static SWIFIObserver* Create();
	};
}
