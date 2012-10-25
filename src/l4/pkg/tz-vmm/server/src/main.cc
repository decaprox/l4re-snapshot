

#include <l4/re/env>
#include <l4/re/util/env_ns>
#include <l4/re/c/namespace.h>
#include <l4/re/dataspace>
#include <l4/re/c/rm.h>
#include <l4/re/c/util/cap_alloc.h>
#include <l4/sys/factory>
#include <l4/sys/ipc.h>
#include <l4/sys/types.h>
#include <l4/sys/vm>
#include <l4/sigma0/sigma0.h>
#include <l4/util/util.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "aisptz.h"



class Vmm {

	private:

		l4_vm_state *_state;
		L4::Cap<L4::Vm> _vm;
		L4::Cap<void> _sigma0;
		L4::Cap<L4Re::Dataspace> _vm_ds;

		l4_addr_t _vm_virt_base;
		l4_addr_t _vm_phys_base;

		l4_fpage_t _fp_state;
		l4_umword_t _label;

		void load_image(const char *image, l4_addr_t addr) 
		{

			L4Re::Util::Env_ns ns;
			L4::Cap<L4Re::Dataspace> ds;
			
			printf("Loading image %s... ", image);

			if (!(ds = ns.query<L4Re::Dataspace>(image))) {
				printf("Image \"%s\" not found!\n", image);
				exit(-1);
			}

			l4_addr_t ds_addr = 0;

			if((L4Re::Env::env()->rm()->attach(&ds_addr, ds->size(),
					            L4Re::Rm::Search_addr, ds)) < 0) {
				printf("Cannot attach image dataspace!\n");
				exit(-1);
			}

			memcpy((void *)addr, (void *)ds_addr, ds->size());

			printf("done.\n");

			L4Re::Env::env()->rm()->detach((void *)ds_addr, &ds);
		}


	public:

		enum {
			Ram_base = 0x80000000,
			Image_start = 0x81000000,
		};


		Vmm(const char *image, unsigned int size) : _label(0) 
		{
			// L4::Vm object creation
			_vm = L4Re::Util::cap_alloc.alloc<L4::Vm>();	
			if (!_vm.is_valid()) {
				printf("Vm capability alloation failed!\n");
				return;
			}

			l4_msgtag_t msg = L4Re::Env::env()->factory()->create_vm(_vm);
			if (l4_error(msg)) {
			printf("Vm creation failed!\n");
				return;
			}

			// Nonsecure Ram mapping
			_sigma0 = L4Re::Env::env()->get_cap<void>("sigma0");
			if (!_sigma0.is_valid()) {
				printf("Sigma0 capability getting failed!\n");
				exit(-1);
			}

			size = l4_trunc_page(size);

			int err = L4Re::Env::env()->rm()->reserve_area(&_vm_virt_base, size,
					L4Re::Rm::Search_addr, 20);
			if (err < 0) {
				printf("Arrea reserving failed (%d)\n", err);
				exit (-1);
			}

			err = l4sigma0_map_iomem(_sigma0.cap(), Ram_base, _vm_virt_base, size, 1);
			if (err) {
				printf("Nonsecure Ram mapping failed!\n");
				exit(-1);
			}

			load_image(image, _vm_virt_base + (Image_start - Ram_base)); 

			posix_memalign((void **)&_state, L4_PAGESIZE, sizeof(l4_vm_state));
			_fp_state = l4_fpage((unsigned long)_state, 12, 0);


		}

		void init() 
		{
			_state->pc = Image_start;
			_state->cpsr = 0x13;
		}

		l4_msgtag_t run() { return _vm->run(_fp_state, &_label); }

		void handle_smc()
		{
			// TODO
		
		}

		void dump(void)
		{
			printf("\npc:%08lx cpsr:%08lx\n",
			    _state->pc, _state->cpsr);
			printf("r0:%08lx r1:%08lx r2:%08lx r3:%08lx\n",
			    _state->r[0], _state->r[1], _state->r[2], _state->r[3]);
			printf("r4:%08lx r5:%08lx r6:%08lx r7:%08lx\n",
			    _state->r[4], _state->r[5], _state->r[6], _state->r[7]);
			printf("r8:%08lx r9:%08lx r10:%08lx r11:%08lx\n",
			    _state->r[8], _state->r[9], _state->r[10], _state->r[11]);
			printf("r12:%08lx\n",
			    _state->r[12]);
			printf("lr_svc:%08lx sp_svc:%08lx spsr_svc:%08lx\n",
			    _state->lr_svc, _state->sp_svc, _state->spsr_svc);
			printf("exit_reason:%08lx\n");
		}

};



int
main(void)
{
	printf("Hello from tz-vmm!\n");

	Vmm vmm("rom/normal.bin", 0x4000000);

	vmm.init();

	printf("Starting vm... \n");
	while (true) {
		l4_msgtag_t	msg = vmm.run();
		if (l4_error(msg)) {
			printf("Vm run failed\n");
			vmm.dump();
		}
		vmm.handle_smc();
		vmm.dump();
	}

}
