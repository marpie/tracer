#include <assert.h>
#include <signal.h>

#include "files.h"
#include "fuzzybool.h"
#include "logging.h"
#include "dmalloc.h"
#include "stuff.h"
#include "rbtree.h"
#include "porg_utils.h"
#include "memorycache.h"
#include "opts.h"
#include "cycle.h"
#include "thread.h"
#include "process.h"
#include "CONTEXT_utils.h"
#include "utils.h"
#include "BP.h"
#include "bp_address.h"

rbtree *processes=NULL; // PID, ptr to process

bool tracer_c_debug=true;
bool detaching=false;

void help_and_exit()
{
    printf ("help...\n");
    exit(0);
};

void load_process()
{
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    strbuf cmd_line=STRBUF_INIT;
    DWORD flags;

    strbuf_addstr (&cmd_line, load_filename);

    if (load_command_line)
    {
        strbuf_addc (&cmd_line, ' ');
        strbuf_addstr (&cmd_line, load_command_line);
    };

    GetStartupInfo (&si);

    if (debug_children)
        flags=DEBUG_PROCESS | CREATE_NEW_CONSOLE;
    else
        flags=DEBUG_ONLY_THIS_PROCESS | CREATE_NEW_CONSOLE;

    if (CreateProcess (load_filename, (LPSTR)cmd_line.buf, 0, 0, 0, flags, 0, 0, &si, &pi)==FALSE)
    {
        if (GetLastError()==ERROR_ELEVATION_REQUIRED)
            die ("UAC issues: run 'as Administrator'\n");
        die_GetLastError ("CreateProcess failed");
    };
    strbuf_deinit (&cmd_line);
};

void attach_process(obj *PIDs)
{
    for (obj *i=PIDs; i; i=cdr(i))
    {
        if (DebugActiveProcess (obj_get_as_tetrabyte(car(i)))==FALSE)
            die_GetLastError ("DebugActiveProcess() failed\n");
    };
};

void clean_all_DRx()
{
    if (tracer_c_debug)
        L ("%s()\n", __func__);

    for (struct rbtree_node_t *_p=rbtree_minimum(processes); _p; _p=rbtree_succ(_p))
    {
        process *p=(process*)(_p->value);

        for (struct rbtree_node_t *_t=rbtree_minimum(p->threads); _t; _t=rbtree_succ(_t))
        {
            thread *t=(thread*)(_t->value);
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_ALL;
            DWORD tmpd;
            tmpd=GetThreadContext (t->THDL, &ctx); assert (tmpd!=FALSE);           

            ctx.Dr0=ctx.Dr1=ctx.Dr2=ctx.Dr3=ctx.Dr7=0;

            tmpd=SetThreadContext (t->THDL, &ctx); assert (tmpd!=FALSE);
        };
    };
};

void detach()
{
    L ("Detaching...\n");
    detaching=true;
};

void debug_or_attach()
{
    obj* attach_PIDs=NULL;

    if (load_filename)
    {
        load_process();
    } else if (attach_filename)
    {
        attach_PIDs=FindProcessByName(attach_filename);
        if (attach_PIDs==NULL)
            die ("Can't find process named %s\n", attach_filename);
        attach_process(attach_PIDs);
        obj_free(attach_PIDs);
    } else if (attach_PID!=-1)
    {
        attach_PIDs=cons(obj_tetrabyte(attach_PID), NULL);
        attach_process(attach_PIDs);
        obj_free(attach_PIDs);
    }
    else
        die ("No program specified for load or attach\n");
};

void check_option_constraints()
{
    if (load_filename && (attach_filename || attach_PID!=-1))
        die ("Load process and attach process options shouldn't be mixed\n");
    if (load_filename==NULL)
    {
        if (load_command_line)
            die ("-c options useless without -l option\n");
        if (debug_children)
            die ("--child option useless without -l option\n");
        if (OEP_breakpoint)
            die ("OEP breakpoint is useless without -l option\n");
    };
};

void print_stack_for_all_processes_and_threads()
{
    for (struct rbtree_node_t *_p=rbtree_minimum(processes); _p; _p=rbtree_succ(_p))
    {
        process *p=(process*)(_p->value);

        for (struct rbtree_node_t *_t=rbtree_minimum(p->threads); _t; _t=rbtree_succ(_t))
        {
            thread *t=(thread*)(_t->value);
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_ALL;
            DWORD tmpd;
            tmpd=GetThreadContext (t->THDL, &ctx); 
            if (tmpd==FALSE)
                die_GetLastError ("GetThreadContext() failed\n");

            MemoryCache *mc=MC_MemoryCache_ctor (p->PHDL, false);

            if (rbtree_count(processes)>1)
                L ("PID=%d\n", p->PID);
            if (rbtree_count(p->threads)>1)
                L ("TID=%d\n", t->TID);
            dump_stack_EBP_frame (p, t, &ctx, mc);

            MC_MemoryCache_dtor(mc, true);
        };
    };
};

static void WINAPI thread_B (DWORD param) 
{
    HANDLE hConsoleInput=GetStdHandle (STD_INPUT_HANDLE);

    if (hConsoleInput!=INVALID_HANDLE_VALUE)
    {
        while (TRUE)
        {
            DWORD inp_read;
            INPUT_RECORD inp_record;
            if (ReadConsoleInput (hConsoleInput, &inp_record, 1, &inp_read)!=FALSE)
            {
                if (inp_record.EventType==KEY_EVENT)
                    if (inp_record.Event.KeyEvent.bKeyDown==FALSE)
                    {
                        if (inp_record.Event.KeyEvent.wVirtualKeyCode==VK_ESCAPE)
                        {
                            L ("ESC pressed...\n");
                            detach();
                        };

                        if (inp_record.Event.KeyEvent.wVirtualKeyCode==VK_SPACE) // VK_F1/etc also available
                            print_stack_for_all_processes_and_threads();
                    };
            }
            else
                return;
        };
    }
    else
        return;
};

static void __cdecl signal_handler (int signo)
{
    if (signo==SIGINT)
    {
        L ("Ctrl-C or Ctrl-Break pressed\n");
        detach();
    };
};

static DWORD thread_B_id;
static HANDLE thread_B_handle;
BOOL (WINAPI * DebugActiveProcessStop_ptr)(DWORD pid)=NULL;

void detach_from_all_processes()
{
    if (tracer_c_debug)
        L ("%s() begin\n", __func__);

    for (struct rbtree_node_t *_p=rbtree_minimum(processes); _p; _p=rbtree_succ(_p))
    {
        process *p=(process*)(_p->value);

        if (DebugActiveProcessStop_ptr!=NULL)
        {
            if ((*DebugActiveProcessStop_ptr) (p->PID)==FALSE)
                die_GetLastError ("DebugActiveProcessStop() failed");
        }
        else
        {
            L ("kernel32.dll!DebugActiveProcessStop() was not found, we have to kill process (PID=%d)\n", p->PID);
            BOOL b=TerminateProcess (p->PHDL, 0);
            assert (b);
        };
    };
};

strbuf ORACLE_HOME;
int oracle_version=-1; // -1 mean 'unknown'

void set_ORACLE_HOME()
{
    strbuf_init (&ORACLE_HOME, 0);
    char *tmp=getenv("ORACLE_HOME");
    if (tmp==NULL)
        return;
    L ("ORACLE_HOME is set to [%s]\n", tmp);

    strbuf_addstr(&ORACLE_HOME, tmp);
    if (strbuf_last_char(&ORACLE_HOME)!='\\')
        strbuf_addc(&ORACLE_HOME, '\\');

    strbuf tmp2;

    strbuf_init(&tmp2, 0);
    strbuf_addf (&tmp2, "%sBIN\\oravsn11.dll", ORACLE_HOME.buf);
    if (file_exist(tmp2.buf))
    {
        L ("Oracle RDBMS version 11.x\n");
        oracle_version=11;
    };
    strbuf_deinit(&tmp2);

    strbuf_init(&tmp2, 0);
    strbuf_addf (&tmp2, "%sBIN\\oravsn10.dll", ORACLE_HOME.buf);
    if (file_exist(tmp2.buf))
    {
        L ("Oracle RDBMS version 10.x\n");
        oracle_version=10;
    };
    strbuf_deinit(&tmp2);

    strbuf_init(&tmp2, 0);
    strbuf_addf (&tmp2, "%sBIN\\oravsn9.dll", ORACLE_HOME.buf);
    if (file_exist(tmp2.buf))
    {
        L ("Oracle RDBMS version 9.x\n");
        oracle_version=9;
    };
    strbuf_deinit(&tmp2);

    if (oracle_version==-1)
        L ("Warning: Oracle RDBMS version wasn't determined\n");
};

int main(int argc, char *argv[])
{
    //dmalloc_break_at_seq_n (70);
    
    if (argc==1)
        help_and_exit();

    for (int i=1; i<argc; i++)
        parse_option(argv[i]);

    check_option_constraints();

    L_init ("tracer.log");
    set_ORACLE_HOME();

    for (int i=0; i<4; i++)
        L ("DRx_breakpoints[%d]=0x%p\n", i, DRx_breakpoints[i]);

    debug_or_attach();
    processes=rbtree_create(true, "processes", compare_tetrabytes);
   
    if (run_thread_b && IsDebuggerPresent()==FALSE) // do not start thread B if gdb is used...
    {
        thread_B_handle=CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)thread_B, (PVOID)0, 0, &thread_B_id);
        if (thread_B_handle==NULL)
            die ("Cannot create thread B");
    };
    signal(SIGINT, &signal_handler);
    
    if (EnableDebugPrivilege (TRUE)==FALSE)
        die_GetLastError ("EnableDebugPrivilege() failed\n");
   
    DebugActiveProcessStop_ptr=(BOOL (WINAPI *)(DWORD))(GetProcAddress (LoadLibrary ("kernel32.dll"), "DebugActiveProcessStop"));
    if (DebugActiveProcessStop_ptr==NULL)
        L ("DebugActiveProcessStop() was not found in your kernel32.dll. Detach wouldn't be possible.\n");

    cycle();

    detach_from_all_processes();

    // any left processes?
    rbtree_foreach(processes, NULL, NULL, (void(*)(void*))process_free);

    rbtree_deinit(processes);

    dlist_free(addresses_to_be_resolved, NULL);
    BP_free(OEP_breakpoint);
    for (unsigned i=0; i<4; i++)
        BP_free(DRx_breakpoints[i]);

    DFREE(load_filename);
    DFREE(attach_filename);
    DFREE(load_command_line);
    if (dump_all_symbols_re)
        regfree (dump_all_symbols_re);

    strbuf_deinit(&ORACLE_HOME);
    dump_unfreed_blocks();
    return 0;
};
