/*****************************************************************************\
* Qemu Simulation Framework (qsim)                                            *
* Qsim is a modified version of the Qemu emulator (www.qemu.org), coupled     *
* a C++ API, for the use of computer architecture researchers.                *
*                                                                             *
* This work is licensed under the terms of the GNU GPL, version 2. See the    *
* COPYING file in the top-level directory.                                    *
\*****************************************************************************/
#include <string>
#include <sstream>
#include <iostream>
#include <fstream>
#include <vector>
#include <queue>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#include "qsim.h"
#include "mgzd.h"
#include "qsim-vm.h"

using namespace Qsim;

using std::queue;        using std::vector;        using std::ostream;
using std::stringstream; using std::istringstream; using std::ostringstream;
using std::string;       using std::cerr;          using std::vector;
using std::ofstream;     using std::ifstream;      using std::istream;

template <typename T> static inline void read_header_field(FILE*    f, 
                                                           uint64_t offset, 
                                                           T&       field) 
{
  fseek(f, offset, SEEK_SET);
  fread(&field, sizeof(T), 1, f);
}

static inline void read_data_chunk(FILE*    f, 
                                   uint64_t offset,
                                   uint8_t* ptr, 
                                   size_t   size) 
{ 
  fseek(f, offset, SEEK_SET);
  fread(ptr, size, 1, f);
}

// Put the vtable for Cpu here.
Qsim::Cpu::~Cpu() {}

void Qsim::QemuCpu::load_linux(const char* bzImage) {
  // Open bzImage
  FILE *f = fopen(bzImage, "r");
  if (!f) {
    cerr << "Could not open Linux kernel at " << bzImage << ".\n";
    exit(1);
  }

  // Load Linux kernel from file.
  uint8_t  setup_sects;
  uint32_t syssize_16;
  uint64_t pref_address;

  read_header_field(f, 0x1f1, setup_sects );
  read_header_field(f, 0x1f4, syssize_16  );
  read_header_field(f, 0x258, pref_address);
  

  read_data_chunk(f, 
                  0x0000, 
                  ramdesc->low_mem_ptr + 0x10000 - 0x200, 
                  setup_sects*512 + 512);
  read_data_chunk(f, 
                  setup_sects*512 + 512, 
                  ramdesc->below_4g_ptr, 
                  syssize_16*16);

  // Set CPU registers to boot linux kernel.
  set_reg(QSIM_RIP, 0x0000 );
  set_reg(QSIM_CS,  0x1000 );
  set_reg(QSIM_DS,  0x1000 - 0x20);
  set_reg(QSIM_RSP, 0x1000 );
  set_reg(QSIM_SS,  0x200  );

  // Close bzImage
  fclose(f);
}

void Qsim::QemuCpu::save_state(ostream &o) {
  // Save all of the registers.
  for (int i = 0; i < QSIM_N_REGS; i++) {
    uint64_t contents = get_reg(regs(i));
    o.write((char*)&contents, sizeof(contents));
  }
}

void Qsim::QemuCpu::load_and_grab_pointers(const char* libfile) {
  // Load the library file                                           
  qemu_lib = Mgzd::open(libfile);
  
  //Get the symbols           
  Mgzd::sym(qemu_init,            qemu_lib, "qemu_init"           );
  Mgzd::sym(qemu_run,             qemu_lib, "run"                 );
  Mgzd::sym(qemu_interrupt,       qemu_lib, "interrupt"           );
  Mgzd::sym(qemu_set_atomic_cb,   qemu_lib, "set_atomic_cb"       );
  Mgzd::sym(qemu_set_inst_cb,     qemu_lib, "set_inst_cb"         );
  Mgzd::sym(qemu_set_int_cb,      qemu_lib, "set_int_cb"          );
  Mgzd::sym(qemu_set_mem_cb,      qemu_lib, "set_mem_cb"          );
  Mgzd::sym(qemu_set_magic_cb,    qemu_lib, "set_magic_cb"        );
  Mgzd::sym(qemu_set_io_cb,       qemu_lib, "set_io_cb"           );
  Mgzd::sym(qemu_set_reg_cb,      qemu_lib, "set_reg_cb"          );
  Mgzd::sym(ramdesc_p,            qemu_lib, "qsim_ram"            );
  Mgzd::sym(qemu_get_reg,         qemu_lib, "get_reg"             );
  Mgzd::sym(qemu_set_reg,         qemu_lib, "set_reg"             );
  Mgzd::sym(qemu_mem_rd,          qemu_lib, "mem_rd"              );
  Mgzd::sym(qemu_mem_wr,          qemu_lib, "mem_wr"              );
  Mgzd::sym(qemu_mem_rd_virt,     qemu_lib, "mem_rd_virt"         );
  Mgzd::sym(qemu_mem_wr_virt,     qemu_lib, "mem_wr_virt"         );
}

Qsim::QemuCpu::QemuCpu(int id, 
		       const char* kernel, 
		       unsigned ram_mb) : cpu_id(id), ram_size_mb(ram_mb)
{
  std::ostringstream ram_size_ss; ram_size_ss << ram_mb << 'M';

  // Load the library file and get pointers
  load_and_grab_pointers("./libqemu.so");

  // Initialize Qemu library
  qemu_init(NULL, ram_size_ss.str().c_str(), id);
  ramdesc = *ramdesc_p;

  // Load the Linux kernel
  load_linux(kernel);

  // Set initial values for registers.
  set_reg(QSIM_RIP, 0x0000       );
  set_reg(QSIM_CS,  0x1000       );
  set_reg(QSIM_DS,  0x1000 - 0x20);
  set_reg(QSIM_RSP, 0x1000       );
  set_reg(QSIM_SS,  0x200        );

  // Initialize mutexes.
  pthread_mutex_init(&irq_mutex, NULL);
  pthread_mutex_init(&cb_mutex, NULL);
}

Qsim::QemuCpu::QemuCpu(int id, 
		       Qsim::QemuCpu* master_cpu, 
		       unsigned ram_mb) : cpu_id(id), ram_size_mb(ram_mb)
{
  std::ostringstream ram_size_ss; ram_size_ss << ram_mb << 'M';

  load_and_grab_pointers("./libqemu.so");
  qemu_init(master_cpu->ramdesc, ram_size_ss.str().c_str(), id);
  ramdesc = master_cpu->ramdesc;

  // Set initial values for registers.
  set_reg(QSIM_CS,  0x0000);
  set_reg(QSIM_DS,  0x0000);
  set_reg(QSIM_RIP, 0x0000);

  // Initialize mutexes.
  pthread_mutex_init(&irq_mutex, NULL);
  pthread_mutex_init(&cb_mutex, NULL);
}

Qsim::QemuCpu::QemuCpu(int id, istream &file, Qsim::QemuCpu* master_cpu, 
                       unsigned ram_mb)
  : cpu_id(id), ram_size_mb(master_cpu->ram_size_mb)
{
  std::ostringstream ram_size_ss; ram_size_ss << ram_mb << 'M';

  // Load the library file and get pointers.
  load_and_grab_pointers("./libqemu.so");

  // Initialize Qemu library
  qemu_init(master_cpu->ramdesc, ram_size_ss.str().c_str(), id);
  ramdesc = master_cpu->ramdesc;

  // TODO: The following should be moved to a utility function
  for (int i = 0; i < QSIM_N_REGS; i++) {
    uint64_t contents;
    file.read((char*)&contents, sizeof(contents));
    set_reg(regs(i), contents);
  }

  // Initialize mutexes.
  pthread_mutex_init(&irq_mutex, NULL); 
  pthread_mutex_init(&cb_mutex, NULL);
}

Qsim::QemuCpu::QemuCpu(int id, istream &file, unsigned ram_mb) :
  cpu_id(id), ram_size_mb(ram_mb)
{
  std::ostringstream ram_size_ss; ram_size_ss << ram_mb << 'M';

  load_and_grab_pointers("./libqemu.so");

  qemu_init(NULL, ram_size_ss.str().c_str(), id);
  ramdesc = *ramdesc_p;

  // Read RAM state.
  file.read((char*)ramdesc->low_mem_ptr, ramdesc->low_mem_sz);
  file.read((char*)ramdesc->below_4g_ptr, ramdesc->below_4g_sz);
  file.read((char*)ramdesc->above_4g_ptr, ramdesc->above_4g_sz);


  // TODO: The following should be moved to a utility function
  for (int i = 0; i < QSIM_N_REGS; i++) {
    uint64_t contents; 
    file.read((char*)&contents, sizeof(contents));
    set_reg(regs(i), contents);
  }

  pthread_mutex_init(&irq_mutex, NULL);
  pthread_mutex_init(&cb_mutex, NULL);
}

Qsim::QemuCpu::~QemuCpu() {
  // Close the library file
  Mgzd::close(qemu_lib);

  // Destroy the interrupt mutex.
  pthread_mutex_destroy(&irq_mutex);
}

uint16_t Qsim::OSDomain::n = 0;
unsigned Qsim::OSDomain::ram_size_mb = 0;
vector<queue <uint8_t> > Qsim::OSDomain::pending_ipis;
pthread_mutex_t  Qsim::OSDomain::pending_ipis_mutex = PTHREAD_MUTEX_INITIALIZER;
vector<QemuCpu*> Qsim::OSDomain::cpus;
vector<bool> Qsim::OSDomain::idlevec;
vector<uint16_t> Qsim::OSDomain::tids;
vector<bool> Qsim::OSDomain::running;
vector<ostream*> Qsim::OSDomain::consoles;
void (*Qsim::OSDomain::app_start_cb)(int) = NULL;
void (*Qsim::OSDomain::app_end_cb  )(int) = NULL;

Qsim::OSDomain::OSDomain(uint16_t n_, string kernel_path, unsigned ram_mb)
{
  ram_size_mb = ram_mb;

  if (n != 0) {
    cerr << "Tried to create more than one OSDomain. There can be only one!\n";
    exit(1);
  }
 
  n = n_;

  if (n > 0) {
    // Create a master CPU using the given kernel
    cpus.push_back(new QemuCpu(0, kernel_path.c_str(), ram_mb));
    cpus[0]->set_magic_cb(magic_cb);

    // Set master CPU state to "running"
    running.push_back(true);

    // Initialize Linux task ID to zero and idle to true
    tids.push_back(0);
    idlevec.push_back(true);

    // Create an empty pending-ipi queue.
    pending_ipis.push_back(queue<uint8_t>());

    // Create n-1 slave CPUs
    for (unsigned i = 1; i < n; i++) {
      cpus.push_back(new QemuCpu(i, cpus[0], ram_mb));
      cpus[i]->set_magic_cb(magic_cb);
  
      // Set slave CPU state to "not running"
      running.push_back(false);

      // Initialize Linux task ID to zero and idle to true
      tids.push_back(0);
      idlevec.push_back(true);

      // Create an empty pending-ipi queue.
      pending_ipis.push_back(queue<uint8_t>());
    }
  }

  // Keep a copy of the QEMU RAM descriptor.
  ramdesc = cpus[0]->get_ramdesc();
}

// Create an OSDomain from a saved state file.
Qsim::OSDomain::OSDomain(const char* filename) {
  ifstream file(filename);

  uint32_t n_, ram_size_mb_;
  file.read((char*)&n_, sizeof(n_));
  file.read((char*)&ram_size_mb_, sizeof(ram_size_mb_));

  n = n_;
  ram_size_mb = ram_size_mb_;

  // Read CPU states (including RAM state)
  if (n > 0) {
    cpus.push_back(new QemuCpu(0, file, ram_size_mb));
    cpus[0]->set_magic_cb(magic_cb);
    pending_ipis.push_back(queue<uint8_t>());
    tids.push_back(0);
    idlevec.push_back(true);
    running.push_back(true);
    for (unsigned i = 1; i < n; i++) {
      cpus.push_back(new QemuCpu(i, file, cpus[0], ram_size_mb));
      cpus[i]->set_magic_cb(magic_cb);
      pending_ipis.push_back(queue<uint8_t>());
      running.push_back(true);
      tids.push_back(0);
      idlevec.push_back(true);
      running.push_back(true);
    }
  }

  // Get a copy of the RAM descriptor.
  ramdesc = cpus[0]->get_ramdesc();

  // Start the run with a timer interrupt.
  timer_interrupt();

  file.close();
}

void Qsim::OSDomain::save_state(std::ostream &o) {
  // File format:
  //  uint32_t #cores
  //  uint32_t RAM size, MB
  //  CPU STATE[#cores]
  //  Memory state.
  uint32_t n_cores(cpus.size()), ram_mb(ram_size_mb);

  o.write((const char*)&n_cores, sizeof(n_cores));
  o.write((const char*)&ram_size_mb, sizeof(ram_size_mb));

  o.write((const char*)ramdesc.low_mem_ptr, ramdesc.low_mem_sz);
  o.write((const char*)ramdesc.below_4g_ptr, ramdesc.below_4g_sz);
  o.write((const char*)ramdesc.above_4g_ptr, ramdesc.above_4g_sz);

  for (unsigned i = 0; i < n_cores; i++) cpus[i]->save_state(o);
}

void Qsim::OSDomain::save_state(const char* filename) {
  ofstream file(filename);
  save_state(file);
  file.close();
}

int Qsim::OSDomain::get_tid(uint16_t i) {
  if (!running[i]) return -1;
  else return tids[i];
}

Qsim::OSDomain::cpu_mode Qsim::OSDomain::get_mode(uint16_t i) {
  bool prot = (cpus[i]->get_reg(QSIM_CR0))&1;

  return prot?MODE_PROT:MODE_REAL;
}

Qsim::OSDomain::cpu_prot Qsim::OSDomain::get_prot(uint16_t i) {
  bool user = (cpus[i]->get_reg(QSIM_CS))&1;

  return user?PROT_USER:PROT_KERN;
}

unsigned Qsim::OSDomain::run(uint16_t i, unsigned n) {
  // First, if there are any pending IPIs try to clear them.
  pthread_mutex_lock(&pending_ipis_mutex);
  if (!pending_ipis[i].empty()) {
    uint8_t fv = pending_ipis[i].front();
    int     rv = cpus[i]->interrupt(fv);
    if (rv != fv && rv != -1) {
      pending_ipis[i].pop();
      if (rv != 0xef && rv != 0x30) {
	// If it's not a timer interrupt, it's probably an IPI, so do it later.
        pending_ipis[i].push(rv);
      }
    }
  }
  pthread_mutex_unlock(&pending_ipis_mutex);

  if (running[i]) { return cpus[i]->run(n); } 
  else            { return 0;               }
}

void Qsim::OSDomain::connect_console(std::ostream& s) {
  consoles.push_back(&s);
}

void Qsim::OSDomain::timer_interrupt() {
  if (n > 1 && running[0] && running[1]) {
    for (unsigned i = 0; i < n; i++) if (running[i]) {
      cpus[i]->interrupt(0xef);
    }
  } else {
    cpus[0]->interrupt(0x30);
  }
}

void Qsim::OSDomain::set_inst_cb  (inst_cb_t   cb) {
  for (unsigned i = 0; i < n; i++) cpus[i]->set_inst_cb  (cb);
}

void Qsim::OSDomain::set_mem_cb   (mem_cb_t    cb) {
  for (unsigned i = 0; i < n; i++) cpus[i]->set_mem_cb   (cb);
}

void Qsim::OSDomain::set_int_cb   (int_cb_t    cb) {
  for (unsigned i = 0; i < n; i++) cpus[i]->set_int_cb   (cb);
}

void Qsim::OSDomain::set_io_cb    (io_cb_t     cb) {
  for (unsigned i = 0; i < n; i++) cpus[i]->set_io_cb    (cb);
}

void Qsim::OSDomain::set_atomic_cb(atomic_cb_t cb) {
  for (unsigned i = 0; i < n; i++) cpus[i]->set_atomic_cb(cb);
}

void Qsim::OSDomain::set_reg_cb(reg_cb_t cb) {
  for (unsigned i = 0; i < n; i++) cpus[i]->set_reg_cb(cb);
}

void Qsim::OSDomain::set_app_start_cb(void (*fp)(int)) {
  app_start_cb = fp;
}

void Qsim::OSDomain::set_app_end_cb  (void (*fp)(int)) {
  app_end_cb  = fp;
}

Qsim::OSDomain::~OSDomain() {
  // Destroy the callback objects.
  for (unsigned i = 0; i < atomic_cbs.size(); ++i) delete atomic_cbs[i];
  for (unsigned i = 0; i < io_cbs.size(); ++i) delete io_cbs[i];
  for (unsigned i = 0; i < mem_cbs.size(); ++i) delete mem_cbs[i];
  for (unsigned i = 0; i < int_cbs.size(); ++i) delete int_cbs[i];
  for (unsigned i = 0; i < inst_cbs.size(); ++i) delete inst_cbs[i];
  for (unsigned i = 0; i < start_cbs.size(); ++i) delete start_cbs[i];
  for (unsigned i = 0; i < end_cbs.size(); ++i) delete end_cbs[i];
  for (unsigned i = 0; i < magic_cbs.size(); ++i) delete magic_cbs[i];

  // Destroy the CPUs.
  for (unsigned i = 0; i < n; i++) delete cpus[i];
}

std::vector<Qsim::OSDomain::atomic_cb_obj_base*> Qsim::OSDomain::atomic_cbs;
std::vector<Qsim::OSDomain::magic_cb_obj_base*>  Qsim::OSDomain::magic_cbs;
std::vector<Qsim::OSDomain::io_cb_obj_base*>     Qsim::OSDomain::io_cbs;
std::vector<Qsim::OSDomain::mem_cb_obj_base*>    Qsim::OSDomain::mem_cbs;
std::vector<Qsim::OSDomain::int_cb_obj_base*>    Qsim::OSDomain::int_cbs;
std::vector<Qsim::OSDomain::inst_cb_obj_base*>   Qsim::OSDomain::inst_cbs;
std::vector<Qsim::OSDomain::reg_cb_obj_base*>    Qsim::OSDomain::reg_cbs;
std::vector<Qsim::OSDomain::start_cb_obj_base*>  Qsim::OSDomain::start_cbs;
std::vector<Qsim::OSDomain::end_cb_obj_base*>    Qsim::OSDomain::end_cbs;

int Qsim::OSDomain::atomic_cb(int cpu_id) {
  std::vector<atomic_cb_obj_base*>::iterator i;

  int rval = 0;

  // Logical OR the output of all the registered callbacks. If at least one
  // demands we stop, we must stop.
  for (i = atomic_cbs.begin(); i != atomic_cbs.end(); ++i) {
    if ( (**i)(cpu_id) ) rval = 1;
  }

  return rval;
}

void Qsim::OSDomain::inst_cb(int cpu_id, uint64_t pa, uint64_t va, 
                             uint8_t l, const uint8_t *bytes, 
                             enum inst_type type)
{
  std::vector<inst_cb_obj_base*>::iterator i;

  // Just iterate through the callbacks and call them all.
  for (i = inst_cbs.begin(); i != inst_cbs.end(); ++i)
    (**i)(cpu_id, pa, va, l, bytes, type);
}

void Qsim::OSDomain::mem_cb(int cpu_id, uint64_t pa, uint64_t va,
			   uint8_t s, int type) {
  std::vector<mem_cb_obj_base*>::iterator i;

  for (i = mem_cbs.begin(); i != mem_cbs.end(); ++i)
    (**i)(cpu_id, pa, va, s, type);
}

void Qsim::OSDomain::io_cb(int cpu_id, uint64_t port, uint8_t s, 
			  int type, uint32_t data) {
  std::vector<io_cb_obj_base*>::iterator i;

  for (i = io_cbs.begin(); i != io_cbs.end(); ++i)
    (**i)(cpu_id, port, s, type, data);
}

int Qsim::OSDomain::int_cb(int cpu_id, uint8_t vec) {
  std::vector<int_cb_obj_base*>::iterator i;

  int rval = 0;

  // Logical OR the output of all the registered callbacks.
  for (i = int_cbs.begin(); i != int_cbs.end(); ++i)
    if ((**i)(cpu_id, vec)) rval = 1;

  return rval;
}

void Qsim::OSDomain::reg_cb(int cpu_id, int reg, uint8_t size, int type) {
  std::vector<reg_cb_obj_base*>::iterator i;
  for (i = reg_cbs.begin(); i != reg_cbs.end(); ++i)
    (**i)(cpu_id, reg, size, type);
}

int Qsim::OSDomain::magic_cb(int cpu_id, uint64_t rax) {
  static int waiting_for_eip = -1;

  int rval = 0;
  
  // Start by calling other registered magic instruction callbacks. 
  std::vector<magic_cb_obj_base*>::iterator i;
 
  for (i = magic_cbs.begin(); i != magic_cbs.end(); ++i)
    if ((**i)(cpu_id, rax)) rval = 1;

  if (waiting_for_eip != -1) {
    cpus[waiting_for_eip]->set_reg(QSIM_CS, rax>>4);
    running[waiting_for_eip] = true;
    waiting_for_eip = -1;
    return rval;
  }

  // If this is a "CD Ignore" magic instruction, ignore it.
  if ((rax&0xffff0000) == 0xcd160000) return rval;

  // Look up CPU
  Qsim::QemuCpu *cpu = cpus[cpu_id];
 
  // Take appropriate action
  if ( (rax&0xffffff00) == 0xc501e000 ) {
    // Console output
    static string s = "";
    char c = rax & 0xff;
    if (isprint(c)) {
      s += c;
    }
    if (c == '\n') {
      std::vector<std::ostream *>::iterator i;
      for (i = consoles.begin(); i != consoles.end(); i++) {
	**i << s << '\n';
      }
      s = "";
    }
  } else if ( (rax & 0xffffffff) == 0x1d1e1d1e ) {
    // This CPU is now in the idle loop.
    idlevec[cpu_id] = true;
  } else if ( (rax & 0xffff0000) == 0xc75c0000 ) {
    // Context switch
    idlevec[cpu_id] = false;
    tids[cpu_id] = rax&0xffff;
  } else if ( (rax & 0xffff0000) == 0xb0070000 ) {
    // CPU bootstrap
    waiting_for_eip = rax&0xffff;
  } else if ( (rax & 0xff000000) == 0x1d000000 ) {
    // Inter-processor interrupt
    uint16_t cpu = (rax & 0x00ffff00)>>8;
    uint8_t  vec = (rax & 0x000000ff);
    int v = cpus[cpu]->interrupt(vec);
    
    if (v != -1 && v != 0xef && v != 0x30) {
      pthread_mutex_lock(&pending_ipis_mutex);
      pending_ipis[cpu].push((uint8_t)v);
      pthread_mutex_unlock(&pending_ipis_mutex);
    }
  } else if ( (rax & 0xffffffff) == 0xc7c7c7c7 ) {
    // CPU count request
    cpus[cpu_id]->set_reg(QSIM_RAX, n);
  } else if ( (rax & 0xffffffff) == 0x512e512e ) {
    // RAM size request
    cpus[cpu_id]->set_reg(QSIM_RAX, ram_size_mb);
  } else if ( (rax & 0xffffffff) == 0xaaaaaaaa ) {
    // Application start marker.
    if (app_start_cb) app_start_cb(cpu_id);
     
    std::vector<start_cb_obj_base*>::iterator i;
    for (i = start_cbs.begin(); i != start_cbs.end(); ++i) {
      (**i)(cpu_id);
    }

  } else if ( (rax & 0xffffffff) == 0xfa11dead ) {
    // Shutdown/application end marker.
    if (app_end_cb) app_end_cb(cpu_id);

    std::vector<end_cb_obj_base*>::iterator i;
    for (i = end_cbs.begin(); i != end_cbs.end(); ++i) {
      (**i)(cpu_id);
    }
    for (unsigned i = 0; i < n; i++) running[i] = false;
  } else if ( (rax & 0xfffffff0) != 0x00000000 &&
              (rax & 0xfffffff0) != 0x80000000 &&
              (rax & 0xfffffff0) != 0x40000000 ) {
    // Unknown CPUID
    // Could throw an exception. For now do nothing.
  }

  return rval;
}

std::vector<Queue*> *Qsim::Queue::queues;

Qsim::Queue::Queue(OSDomain &cd, int cpu, bool h): cd(&cd), cpu(cpu), hlt(h) {
  // Create a way for the callbacks to find us.
  if (queues == NULL)
    queues = new vector<Queue*>(cd.get_n());
  (*queues)[cpu] = this;
  // Connect the callbacks
  cd.set_inst_cb(cpu, h?inst_cb_hlt:inst_cb); 
  cd.set_mem_cb (cpu, mem_cb               ); 
  cd.set_int_cb (cpu, int_cb               );
}

Qsim::Queue::~Queue() {
  // Disconnect the callbacks
  cd->set_inst_cb(cpu, NULL);
  cd->set_mem_cb (cpu, NULL);
  cd->set_int_cb (cpu, NULL);

  // Remove this queue from the vector.
  (*queues)[cpu] = NULL;

  // If there are no queues left, delete queues.
  bool queues_all_null = true;
  for (unsigned i = 0; i < cd->get_n(); i++) {
    if ((*queues)[i]) queues_all_null = false;
  }
  if (queues_all_null) delete queues;
}

void Qsim::Queue::set_filt(bool user, bool krnl, bool prot, 
                           bool real, int tid) 
{
  // Set filtering variables
  flt_tid = tid;
  flt_krnl = krnl;
  flt_prot = prot;
  flt_real = real;
  flt_user = user;

  // Set callbacks appropriately
  if (flt_krnl && flt_prot && flt_real && flt_user && flt_tid == -1) {
    cd->set_inst_cb(cpu, hlt?inst_cb_hlt:inst_cb);
    cd->set_mem_cb (cpu, mem_cb                 );
    cd->set_int_cb (cpu, int_cb                 );
  } else {
    cd->set_inst_cb(cpu, inst_cb_flt            );
    cd->set_mem_cb (cpu, mem_cb_flt             );
    cd->set_int_cb (cpu, int_cb_flt             );
  }
}


void Qsim::Queue::inst_cb(int cpu_id, 
			  uint64_t vaddr, 
			  uint64_t paddr, 
			  uint8_t len, 
			  const uint8_t *bytes,
                          enum inst_type type)
{
  (*queues)[cpu_id]->push(QueueItem(vaddr, paddr, len, bytes));
}

void Qsim::Queue::inst_cb_hlt(int cpu_id, 
   			      uint64_t vaddr, 
			      uint64_t paddr, 
			      uint8_t len, 
			      const uint8_t *bytes,
                              enum inst_type type)
{
  if (len == 1 && *bytes == 0xf4) (*queues)[cpu_id]->cd->timer_interrupt();
  (*queues)[cpu_id]->push(QueueItem(vaddr, paddr, len, bytes));
}

void Qsim::Queue::inst_cb_flt(int cpu_id, 
			      uint64_t vaddr, 
  			      uint64_t paddr, 
			      uint8_t len, 
			      const uint8_t *bytes,
                              enum inst_type type)
{
  Queue   *q        = (*queues)[cpu_id];
  OSDomain *cd       = q->cd      ;
  int      cpu      = q->cpu     ;
  int      flt_tid  = q->flt_tid ;
  bool     flt_krnl = q->flt_krnl;
  bool     flt_user = q->flt_user;
  bool     flt_prot = q->flt_prot;
  bool     flt_real = q->flt_real;
  if ( (flt_tid == -1 || cd->get_tid (cpu) == flt_tid           ) && (
         (flt_krnl      && cd->get_prot(cpu) == OSDomain::PROT_KERN) ||
         (flt_user      && cd->get_prot(cpu) == OSDomain::PROT_USER) ||
         (flt_prot      && cd->get_mode(cpu) == OSDomain::MODE_PROT) || 
         (flt_real      && cd->get_mode(cpu) == OSDomain::MODE_REAL)))
    (*queues)[cpu_id]->push(QueueItem(vaddr, paddr, len, bytes));
  if (q->hlt && len == 1 && *bytes == 0xf4) cd->timer_interrupt();
}

void Qsim::Queue::mem_cb(int cpu_id, 
			 uint64_t vaddr, 
			 uint64_t paddr, 
			 uint8_t size, 
			 int type) {
  (*queues)[cpu_id]->push(QueueItem(vaddr, paddr, size, type));
}

void Qsim::Queue::mem_cb_flt(int cpu_id, 
 			     uint64_t vaddr, 
			     uint64_t paddr, 
			     uint8_t size, 
			     int type) {
  Queue   *q        = (*queues)[cpu_id];
  OSDomain *cd       = q->cd      ;
  int      cpu      = q->cpu     ;
  int      flt_tid  = q->flt_tid ;
  bool     flt_krnl = q->flt_krnl;
  bool     flt_user = q->flt_user;
  bool     flt_prot = q->flt_prot;
  bool     flt_real = q->flt_real;
  if ( (flt_tid == -1 || cd->get_tid (cpu) == flt_tid           ) && (
         (flt_krnl      && cd->get_prot(cpu) == OSDomain::PROT_KERN) ||
         (flt_user      && cd->get_prot(cpu) == OSDomain::PROT_USER) ||
         (flt_prot      && cd->get_mode(cpu) == OSDomain::MODE_PROT) || 
	 (flt_real      && cd->get_mode(cpu) == OSDomain::MODE_REAL)))
    (*queues)[cpu_id]->push(QueueItem(vaddr, paddr, size, type));
}

int Qsim::Queue::int_cb(int cpu_id, uint8_t vec) {
  (*queues)[cpu_id]->push(QueueItem(vec));
  return 0;
}

int Qsim::Queue::int_cb_flt(int cpu_id, uint8_t vec) {
  Queue   *q        = (*queues)[cpu_id];
  OSDomain *cd       = q->cd      ;
  int      cpu      = q->cpu     ;
  int      flt_tid  = q->flt_tid ;
  bool     flt_krnl = q->flt_krnl;
  bool     flt_user = q->flt_user;
  bool     flt_prot = q->flt_prot;
  bool     flt_real = q->flt_real;
  if ( (flt_tid == -1 || cd->get_tid (cpu) == flt_tid           ) && (
         (flt_krnl      && cd->get_prot(cpu) == OSDomain::PROT_KERN) ||
         (flt_user      && cd->get_prot(cpu) == OSDomain::PROT_USER) ||
         (flt_prot      && cd->get_mode(cpu) == OSDomain::MODE_PROT) || 
         (flt_real      && cd->get_mode(cpu) == OSDomain::MODE_REAL))) 
    (*queues)[cpu_id]->push(QueueItem(vec));
  return 0;
}
