/*
 *  _______                      
 * |__   __|                     
 *    | |_ __ __ _  ___ ___ _ __ 
 *    | | '__/ _` |/ __/ _ \ '__|
 *    | | | | (_| | (_|  __/ |   
 *    |_|_|  \__,_|\___\___|_|   
 *
 * Written by Dennis Yurichev <dennis(a)yurichev.com>, 2013
 *
 * This work is licensed under the Creative Commons Attribution-NonCommercial-NoDerivs 3.0 Unported License. 
 * To view a copy of this license, visit http://creativecommons.org/licenses/by-nc-nd/3.0/.
 *
 */

#include "tracer.h"
#include "oassert.h"
#include "ostrings.h"
#include "BPF.h"
#include "bp_address.h"
#include "dmalloc.h"
#include "BP.h"
#include "process.h"
#include "thread.h"
#include "utils.h"
#include "opts_aux.h"
#include "CONTEXT_utils.h"
#include "bitfields.h"
#include "x86.h"
#include "symbol.h"
#include "cc.h"
#include "stuff.h"
#include "rand.h"
#include "x86_emu.h"
#include "fmt_utils.h"

void BPF_skip_func (struct process *p, int bp_no, CONTEXT *ctx, struct MemoryCache *mc, address ret_adr)
{
#ifdef INT3_DURING_FUNC_SKIP
            p->INT3_DURING_FUNC_SKIP_used[bp_no]=true;
            //L ("ret_adr=0x" PRI_ADR_HEX "\n", ret_adr);
            p->INT3_DURING_FUNC_SKIP_addresses[bp_no]=ret_adr;
            bool b=MC_ReadByte (mc, ret_adr, &p->INT3_DURING_FUNC_SKIP_byte[bp_no]);
            oassert(b && "can't read byte at breakpoint start");
            b=MC_WriteByte (mc, ret_adr, 0xCC);
            oassert(b && "can't write 0xCC byte at breakpoint start");

            if (verbose>0)
            {
                strbuf sb=STRBUF_INIT;
                address PC=CONTEXT_get_PC(ctx);
                process_get_sym (p, PC, /* add_module_name */ true, /* add_offset */ true, &sb);
                L ("(%d) Using INT3 to skip execution of %s\n", bp_no, sb.buf);
                strbuf_deinit(&sb);
            };
#else
            //clear_TF(ctx);
            CONTEXT_setDRx_and_DR7 (ctx, bp_no, ret_adr);
#endif
};

static const char* function_type_ToString (enum function_type f)
{
    switch (f)
    {
        case TY_UNKNOWN: 
            return "unknown";
        case TY_VOID: 
            return "void";
        case TY_UNINTERESTING: 
            return "uninteresting";
        case TY_REG:
            return "REG";
        case TY_INT:
            return "int";
        case TY_PTR:
            return "ptr";
        case TY_TETRABYTE:
            return "tetra";
        case TY_QSTRING:
            return "QString";
        case TY_PTR_TO_QSTRING:
            return "QString*";
        case TY_PTR_TO_DOUBLE:
            return "double*";
        default:
            oassert(0);
    };
};

void BPF_ToString(struct BPF *b, strbuf *out)
{
    strbuf_addstr (out, "BPF. options: ");
    if (b->unicode)
        strbuf_addstr (out, "unicode ");
    if (b->microsoft_fastcall)
        strbuf_addstr (out, "microsoft_fastcall ");
    if (b->borland_fastcall)
        strbuf_addstr (out, "borland_fastcall ");
    if (b->skip)
        strbuf_addstr (out, "skip ");
    if (b->skip_stdcall)
        strbuf_addstr (out, "skip_stdcall ");
    if (b->trace)
        strbuf_addstr (out, "trace ");
    if (b->cc)
        strbuf_addstr (out, "cc ");
    strbuf_addf (out, "rt: " PRI_REG_HEX " ", b->rt);

    //fprintf (stderr, "%s() rt_probability: %f ", __func__, b->rt_probability);
    if (b->rt_probability!=0 && b->rt_probability!=1)
        strbuf_addf (out, "rt_probability: %f ", b->rt_probability);

    if (b->args)
        strbuf_addf (out, "args: %d ", b->args);

    if (b->dump_args)
        strbuf_addf (out, "dump_args: %d ", b->dump_args);

    if (b->pause)
        strbuf_addf (out, "pause: %d ", b->pause);

    strbuf_addstr (out, "\n");
    if (b->when_called_from_address)
    {
        strbuf_addstr (out, "when_called_from_address: ");
        address_to_string (b->when_called_from_address, out);
        strbuf_addstr (out, "\n");
    };
    if (b->when_called_from_func)
    {
        strbuf_addstr (out, "when_called_from_func: ");
        address_to_string (b->when_called_from_func, out);
        strbuf_addstr (out, "\n");
    };
    if (b->set_present)
    {
        strbuf_addf (out, "set_width=%d set_arg_n=%d set_ofs=0x" PRI_REG_HEX " set_val=0x" PRI_REG_HEX "\n", b->set_width, b->set_arg_n, b->set_ofs, b->set_val);
    };
    // NOTE: args_n, arg_types, ret_type, this_type not dumped
    if (b->arg_types)
        for (int i=0; i<b->args; i++)
            strbuf_addf(out, "arg%d_type=%s ", i+1, function_type_ToString(b->arg_types[i]));
};

void BPF_free(struct BPF* o)
{
    if (o->when_called_from_address)
        bp_address_free(o->when_called_from_address);
    if (o->when_called_from_func)
        bp_address_free(o->when_called_from_func);
    DFREE (o->arg_types);
    DFREE (o);
};

struct QString_obj
{
    REG QBasicAtomicInt;
    tetra alloc;
    tetra size;
    address data;
};

static void dump_QString (address a, struct MemoryCache *mc)
{
    /*
       struct Data {
       QBasicAtomicInt ref;
       int alloc, size;
       ushort *data; // QT5: put that after the bit field to fill alignment gap; don't use sizeof any more then
       ushort clean : 1;
       ushort simpletext : 1;
       ushort righttoleft : 1;
       ushort asciiCache : 1;
       ushort capacity : 1;
       ushort reserved : 11;
    // ### Qt5: try to ensure that "array" is aligned to 16 bytes on both 32- and 64-bit
    ushort array[1];
    };
    */
    //REG r2;
    struct QString_obj o;
    L ("QString: ");
    if (MC_ReadBuffer (mc, a, sizeof(struct QString_obj), &o))
    {
        strbuf sb=STRBUF_INIT;
        if (MC_GetString (mc, o.data, /* unicode */ true, &sb))
        {
            L ("(data=\"%s\")", sb.buf);
            // replace string with mine
            //wchar_t* newstr=L"...";
            //MC_WriteBuffer(mc, o.data, (wcslen(newstr)+1)*sizeof(wchar_t), newstr);
        }
        else
        {
            BYTE buf[4];
            if (MC_ReadBuffer (mc, o.data, 4, buf))
                L ("(data=(0x%x 0x%x 0x%x 0x%x ... ))", buf[0], buf[1], buf[2], buf[3]);
            else
                L ("(data=(read error))");
        };
        
        strbuf_deinit(&sb);
    }
    else
        L ("(object read error)");
};

// function to be separated?
// return true if hexadecimal number dumped
static bool BPF_dump_arg (struct MemoryCache *mc, REG arg, bool unicode, enum function_type ft)
{
    bool rt=false;
    switch (ft)
    {
        case TY_UNKNOWN:
            {
                strbuf sb=STRBUF_INIT;

                if (MC_GetString(mc, arg, unicode, &sb) && sb.strlen>2)
                    L ("\"%s\"", sb.buf);
                else
                {
                    L ("0x" PRI_REG_HEX, arg);
                    rt=true;
                };

                strbuf_deinit (&sb);
            };
            break;

        case TY_QSTRING:
            dump_QString(arg, mc);
            break;
        
        case TY_PTR_TO_QSTRING:
            {
                address tmp;
                if (MC_ReadREG (mc, arg, &tmp))
                    dump_QString(tmp, mc);
                else
                    L ("(while dumping TY_PTR_TO_QSTRING: cannot read at 0x" PRI_ADR_HEX "\n", arg);
            };
            break;

        case TY_PTR_TO_DOUBLE:
            {
                double tmp;
                if (MC_ReadBuffer (mc, arg, sizeof(double), &tmp))
                    L ("%f", tmp);
                else
                    L ("(while dumping TY_PTR_TO_DOUBLE: cannot read at 0x" PRI_ADR_HEX "\n", arg);
            };
            break;

        case TY_PTR: // get sym?
        case TY_REG:
        case TY_INT:
            L ("0x" PRI_REG_HEX, arg);
            break;

        case TY_VOID:
        case TY_UNINTERESTING:
            break;

        default:
            printf ("ft=%d\n", ft);
            oassert(!"not implemented");
            break;
    };
    return rt;
};

static void BPF_dump_args (struct MemoryCache *mc, REG* args, unsigned args_n, bool unicode, enum function_type *arg_types)
{
    for (unsigned i=0; i<args_n; i++)
    {
        BPF_dump_arg (mc, args[i], unicode, arg_types ? arg_types[i] : TY_UNKNOWN);
        if ((i+1) != args_n)
            L (", ");
    };
};

#ifdef _WIN64
static bool read_MS_x64_arguments (struct MemoryCache *mc, CONTEXT *ctx, unsigned args,
        struct BP_thread_specific_dynamic_info *di)
{
    for (unsigned a=0; a<args; a++)
    {
        switch (a)
        {
            case 0: di->BPF_args[a]=ctx->Rcx;
                    break;
            case 1: di->BPF_args[a]=ctx->Rdx;
                    break;
            case 2: di->BPF_args[a]=ctx->R8;
                    break;
            case 3: di->BPF_args[a]=ctx->R9;
                    break;
            default:
                    if (read_argument_from_stack (mc, ctx, a+4, &di->BPF_args[a])==false)
                        return false;
                    break;
        };
    };
    return true;
};
#endif

#ifndef _WIN64
static bool read_microsoft_fastcall_arguments (struct MemoryCache *mc, CONTEXT *ctx, unsigned args,
        struct BP_thread_specific_dynamic_info *di)
{
    unsigned arguments_in_stack=(args<2) ? 0 : args-2;

    for (unsigned a=0; a<args; a++)
    {
        switch (a)
        {
            case 0: di->BPF_args[a]=ctx->Ecx;
                    break;
            case 1: di->BPF_args[a]=ctx->Edx;
                    break;
            default:
                    if (read_argument_from_stack (mc, ctx, arguments_in_stack - (a-2), &di->BPF_args[a])==false)
                        return false;
                    break;
        };
    };
    return true;
};

static bool read_borland_fastcall_arguments (struct MemoryCache *mc, CONTEXT *ctx, unsigned args,
        struct BP_thread_specific_dynamic_info *di)
{
    unsigned arguments_in_stack=(args<3) ? 0 : args-3;

    for (unsigned a=0; a<args; a++)
    {
        switch (a)
        {
            case 0: di->BPF_args[a]=ctx->Eax;
                    break;
            case 1: di->BPF_args[a]=ctx->Edx;
                    break;
            case 2: di->BPF_args[a]=ctx->Ecx;
                    break;
            default:
                    if (read_argument_from_stack (mc, ctx, arguments_in_stack - (a-3), &di->BPF_args[a])==false)
                        return false;
                    break;
        };
    };
    return true;
};
#endif

static void load_args(struct thread *t, CONTEXT *ctx, struct MemoryCache *mc, unsigned bp_no, unsigned args, struct BPF* bpf)
{
	address SP=CONTEXT_get_SP(ctx);
	struct BP_thread_specific_dynamic_info *di=&t->BP_dynamic_info[bp_no];
	di->BPF_args=DMALLOC(REG, args, "REG");

#ifdef _WIN64
	if (read_MS_x64_arguments (mc, ctx, args, di)==false)
		goto read_failed;
#else
	if (bpf->borland_fastcall)
		{
			if (read_borland_fastcall_arguments (mc, ctx, args, di)==false)
				goto read_failed;
		}
	else
		if (bpf->microsoft_fastcall)
			{
				if (read_microsoft_fastcall_arguments (mc, ctx, args, di)==false)
					goto read_failed;
			}
		else
			{ // default:
				if (MC_ReadBuffer(mc, SP+REG_SIZE, args*REG_SIZE, (BYTE*)di->BPF_args)==false)
					goto read_failed;
			};
#endif
	return;

read_failed:
	DFREE (di->BPF_args);
	di->BPF_args=NULL;
};

static void handle_set (struct BPF* bpf, unsigned bp_no, struct process *p, struct thread *t, CONTEXT *ctx, struct MemoryCache *mc, unsigned args)
{
    struct BP_thread_specific_dynamic_info *di=&t->BP_dynamic_info[bp_no];
    if (bpf->set_arg_n+1 > args)
    {
        L ("Can't work with arg_%d: it wasn't loaded, change 'args' options in BPF!\n", bpf->set_arg_n);
        return;
    };

    address adr=di->BPF_args[bpf->set_arg_n] + bpf->set_ofs;
    bool succ=MC_WriteValue(mc, adr, bpf->set_width, bpf->set_val);

    L ("0x" PRI_REG_HEX " %swritten at address 0x" PRI_ADR_HEX " (arg_%d+0x" PRI_REG_HEX ")\n", 
            bpf->set_val, succ ? "" : "cannot be ", adr, bpf->set_arg_n, bpf->set_ofs);
};

static void dump_bufs_if_need(struct process *p, struct thread *t, struct MemoryCache *mc, unsigned size, unsigned args_n, REG* args, unsigned bp_no)
{
    struct BP_thread_specific_dynamic_info *di=&t->BP_dynamic_info[bp_no];
    oassert (di->BPF_buffers_at_start==NULL);
    di->BPF_buffers_at_start=(BYTE**)DCALLOC(REG*, args_n, "REG*");
    di->BPF_buffers_at_start_cnt=args_n;

    for (unsigned i=0; i<args_n; i++)
    {
        BYTE *buf=DMALLOC(BYTE, size, "buf");
        if (MC_ReadBuffer (mc, args[i], size, buf))
        {
            strbuf sb=STRBUF_INIT;
            strbuf_addf (&sb, "Argument %d/%d ", i+1, args_n);
            L ("%s\n", sb.buf);
            L_print_buf_ofs (buf, size, args[i]);
            print_symbols_in_buf_if_possible (p, mc, buf, size, sb.buf);
            di->BPF_buffers_at_start[i]=buf;
            strbuf_deinit (&sb);
        }
        else
            DFREE(buf);
    };
};

static void dump_args_diff_if_need(struct process *p, struct thread *t, struct MemoryCache *mc, unsigned size, unsigned args_n, REG* args, unsigned bp_no)
{
    struct BP_thread_specific_dynamic_info *di=&t->BP_dynamic_info[bp_no];
    oassert (di->BPF_buffers_at_start);

    for (unsigned i=0; i<args_n; i++)
    {
        if (di->BPF_buffers_at_start[i]==NULL)
            continue;

        BYTE *buf2=DMALLOC(BYTE, size, "buf");
        bool b=MC_ReadBuffer (mc, args[i], size, buf2);
        oassert (b);
        if (memcmp (di->BPF_buffers_at_start[i], buf2, size)!=0)
        {
            L ("Argument %d/%d difference\n", i+1, args_n);
            L_print_bufs_diff (di->BPF_buffers_at_start[i], buf2, size);
            // print symbols in changed buf, if possible
            strbuf sb1=STRBUF_INIT, sb2=STRBUF_INIT;
            strbuf_addf (&sb1, "Argument %d/%d before ", i+1, args_n);
            strbuf_addf (&sb2, "Argument %d/%d after ", i+1, args_n);
            print_symbols_in_intersection_of_bufs (p, mc, 
                    di->BPF_buffers_at_start[i], buf2, sb1.buf, sb2.buf, size);
            strbuf_deinit (&sb1);
            strbuf_deinit (&sb2);
        };
        DFREE(buf2);
        DFREE(di->BPF_buffers_at_start[i]);
    };
    DFREE (di->BPF_buffers_at_start);
    di->BPF_buffers_at_start=NULL;
};
        
static void dump_object_info_if_needed(struct BPF *bpf, struct MemoryCache *mc, CONTEXT *ctx)
{
    if (bpf->this_type==TY_UNKNOWN || bpf->this_type==TY_UNINTERESTING)
        return;

    L ("this=");
    REG r;
    if (MC_ReadREG(mc, CONTEXT_get_xCX(ctx), &r))
        BPF_dump_arg(mc, r, bpf->unicode, bpf->this_type);
    else
        L ("(read error)");
    L ("\n");
};

static void is_it_known_function (const char *fn_name, struct BPF* bpf)
{
    if (verbose>0)
        L ("%s(fn_name=%s)\n", __func__, fn_name);

    if (bpf->arg_types)
        return; // already overriden by BPF option

    // FIXME: demangler should be here!
    if (strstr(fn_name, "?information@QMessageBox@@SAHPEAVQWidget@@AEBVQString@@1111HH@Z"))
    {
        bpf->args=8;
        bpf->arg_types=DCALLOC(enum function_type, bpf->args, "function_type");
        bpf->arg_types[0]=TY_PTR; // QWidget
        bpf->arg_types[1]=bpf->arg_types[2]=bpf->arg_types[3]=bpf->arg_types[4]=bpf->arg_types[5]=TY_QSTRING;
        bpf->arg_types[6]=bpf->arg_types[7]=TY_INT;
        bpf->ret_type=bpf->this_type=TY_UNINTERESTING;
        bpf->known_function=Fuzzy_True;
        if (verbose>0)
            L ("%s() - True\n", __func__);
        return;
    };

    if (strstr(fn_name, "?1QString@@QEAA@XZ") || // in fact: "??1QString@@QEAA@XZ" 
        strstr(fn_name, "?1QString@@QAE@XZ"))    // in fact: "??1QString@@QAE@XZ" __thiscall QString::~QString(void)
    {
        bpf->this_type=TY_QSTRING;
        bpf->known_function=Fuzzy_True;
        if (verbose>0)
            L ("%s() - True\n", __func__);
        return;
    };
    bpf->known_function=Fuzzy_False;
    if (verbose>0)
        L ("%s() - False\n", __func__);
};

// return - should this call skipped?
static bool handle_when_called_from_func(struct process *p, struct thread *t, unsigned bp_no, struct BPF *bpf, bool got_ret_adr)
{
    struct BP_thread_specific_dynamic_info *di=&t->BP_dynamic_info[bp_no];
    oassert (bpf->when_called_from_func);

    if (bpf->when_called_from_func->resolved==false || got_ret_adr==false)
        return true;

    if (bpf->when_called_from_func_next_func_adr_present==false)
    {
        // be sure (once), we work with symbol start
        if (process_sym_exist_at (p, bpf->when_called_from_func->abs_address)==NULL)
            die ("The address set in when_called_from_func should be symbol start!\n");

        address a=process_get_next_sym_address_after (p, bpf->when_called_from_func->abs_address);
        if (a)
        {
            bpf->when_called_from_func_next_func_adr=a;
            if (verbose>0)
                L ("(case 1) bpf->when_called_from_func_next_func_adr=0x" PRI_ADR_HEX "\n", bpf->when_called_from_func_next_func_adr);
        }
        else
        {
            bpf->when_called_from_func_next_func_adr=get_module_end(find_module_for_address (p, bpf->when_called_from_func->abs_address));
            if (verbose>0)
                L ("(case 2) bpf->when_called_from_func_next_func_adr=0x" PRI_ADR_HEX "\n", bpf->when_called_from_func_next_func_adr);
        };
        bpf->when_called_from_func_next_func_adr_present=true;
    };

    if (di->ret_adr>=bpf->when_called_from_func->abs_address && 
            di->ret_adr<bpf->when_called_from_func_next_func_adr)
        return false;

    return true;
};

// return values
// 0 - OK, go ahead
// 1 - when_called_from_func case, or we didn't get return address. do not call handle_finish()
// 2 - this function should be skipped, call handle_finish()
static unsigned handle_begin(struct process *p, struct thread *t, struct BP *bp, int bp_no, CONTEXT *ctx, struct MemoryCache *mc)
{
    // do function begin things
    struct BP_thread_specific_dynamic_info *di=&t->BP_dynamic_info[bp_no];
    struct BPF *bpf=bp->u.bpf;
    struct bp_address *bp_a=bp->a;
    strbuf sb=STRBUF_INIT, sb_address=STRBUF_INIT;
    address_to_string(bp_a, &sb_address); // sb_address in form module!regexp
    address SP=CONTEXT_get_SP(ctx);
    bool got_ret_adr=MC_ReadREG(mc, SP, &di->ret_adr);
    di->SP_at_ret_adr=got_ret_adr ? SP : 0;

    unsigned rt;

    if (bpf->when_called_from_func && handle_when_called_from_func(p, t, bp_no, bpf, got_ret_adr))
    {
        rt=1;
        goto exit;
    };

    if (got_ret_adr)
        process_get_sym (p, di->ret_adr, /* add_module_name */ true, /* add_offset */ true, &sb);
    else
        L ("Cannot read a register at SP, so, BPF return will not be handled\n");

    if (bpf->known_function==Fuzzy_Undefined)
    {
        strbuf sb_symbol_name=STRBUF_INIT;
        
        // FIXME: all symbols at $bp_a->abs_address$ should be enumerated!
        is_it_known_function(sb_address.buf, bpf);

        if (bpf->known_function==Fuzzy_True)
            L ("This is known (to us) function\n");
        strbuf_deinit(&sb_symbol_name);
    };

    unsigned args=bpf->known_function==Fuzzy_True ? bpf->args : bpf->args;

    load_args(t, ctx, mc, bp_no, args, bpf);

    dump_PID_if_need(p); dump_TID_if_need(p, t);
    L ("(%d) %s(", bp_no, sb_address.buf);
    BPF_dump_args (mc, di->BPF_args, args, bpf->unicode, bpf->arg_types);
    L (")");
    if (got_ret_adr)
        L (" (called from %s (0x" PRI_ADR_HEX "))", sb.buf, di->ret_adr);
    L ("\n");

    dump_object_info_if_needed(bpf, mc, ctx);

    if (bpf->dump_args)
        dump_bufs_if_need(p, t, mc, bpf->dump_args, bpf->args, di->BPF_args, bp_no);

    if (dash_s)
        dump_stack (p, t, ctx, mc);
    
    if (bpf->pause)
    {
        L ("Sleeping for %d milliseconds...\n", bpf->pause);
        Sleep (bpf->pause);
    };

    if (bpf->set_present)
        handle_set(bpf, bp_no, p, t, ctx, mc, args);

    if (got_ret_adr)
    {
	    // FIXME: microsoft_fastcall and borland_fastcall (?) should be handled
        if (bpf->skip || bpf->skip_stdcall)
        {
            L ("(%d) Skipping execution of this function\n", bp_no);
            CONTEXT_set_PC(ctx, di->ret_adr);
            unsigned add_to_SP=0;

            if (bpf->skip_stdcall)
                add_to_SP=bpf->args;
            
            CONTEXT_set_SP(ctx, CONTEXT_get_SP(ctx) + (1 + add_to_SP)*sizeof(REG));
            rt=2;
        }
        else
        {
            // set current DRx to return
            CONTEXT_setDRx_and_DR7 (ctx, bp_no, di->ret_adr); // set breakpoint at return
            rt=0;
        }
    }
    else
        rt=1;
exit:
    strbuf_deinit(&sb);
    strbuf_deinit(&sb_address);
    return rt;
};

static void handle_finish(struct process *p, struct thread *t, struct BP *bp, int bp_no, CONTEXT *ctx, struct MemoryCache *mc)
{
    struct BP_thread_specific_dynamic_info *di=&t->BP_dynamic_info[bp_no];
    struct bp_address *bp_a=bp->a;
    struct BPF *bpf=bp->u.bpf;
    strbuf sb_address=STRBUF_INIT;
    address_to_string(bp_a, &sb_address);
    // do function finish things
    REG accum=CONTEXT_get_Accum (ctx);

    // dump all info in one line:
    dump_PID_if_need(p); dump_TID_if_need(p, t);
    L ("(%d) %s() -> ", bp_no, sb_address.buf);

    if (BPF_dump_arg(mc, accum, bpf->unicode, bpf->ret_type)==false)
        L (" (0x" PRI_REG_HEX ")", accum);

#ifdef THIS_CODE_IS_NOT_WORKING
    if (dump_fpu && STx_present_in_tag(ctx, 0))
#endif        
    if (dump_fpu)
        L (", ST0=%.1f", get_STx(ctx, 0));
    
    L ("\n");

    if (bpf->dump_args)
        dump_args_diff_if_need(p, t, mc, bpf->dump_args, bpf->args, di->BPF_args, bp_no);

    DFREE(di->BPF_args); di->BPF_args=NULL;

    bool modify_AX=false;

    if (bpf->rt_present && bpf->rt_probability_present==false)
        modify_AX=true;

    if (bpf->rt_present && bpf->rt_probability_present)
    {
        if (verbose>0)
        {
            L ("bpf->rt_probability=%f\n", bpf->rt_probability);
            L_print_buf ((BYTE*)&bpf->rt_probability, sizeof(double));
        };
        if (rand_double()<bpf->rt_probability)
            modify_AX=true;
    };

    if (modify_AX)
    {
        dump_PID_if_need(p); dump_TID_if_need(p, t);
        L ("(%d) Modifying %s register to 0x%x\n", bp_no, AX_REGISTER_NAME, bpf->rt);
        CONTEXT_set_Accum(ctx, bpf->rt);
    };

    // set back current DRx to begin
    oassert(bp_no<4); // be sure we work only with DRx breakpoints
    CONTEXT_setDRx_and_DR7 (ctx, bp_no, bp_a->abs_address);
    strbuf_deinit(&sb_address);
};

bool handle_tracing_should_be_skipped(struct process *p, struct thread *t, struct MemoryCache *mc, CONTEXT *ctx, unsigned bp_no)
{
    REG PC=CONTEXT_get_PC(ctx);
    DWORD tmp;
    if (MC_ReadTetrabyte (mc, PC, &tmp) && tmp==0xC015FF64) // 64 FF 15 C0 00 00 00 <call large dword ptr fs:0C0h>
    {
        if (verbose>0)
            L ("syscall to be skipped\n");
        CONTEXT_setDRx_and_DR7 (ctx, bp_no, PC+7);
        return true;
    };

    // is there symbol?
    struct module *m=find_module_for_address (p, PC);
    struct symbol *s=process_sym_exist_at(p, PC);
    if (s)
    {
        // should it be skipped?
        if (symbol_skip_on_tracing(m, s))
        {
            REG ret_adr;
            //L ("symbol to be skipped\n");
            if (MC_ReadREG(mc, CONTEXT_get_SP(ctx), &ret_adr)==false)
            {
                oassert(!"can't read return address while we'are 'inside' function");
            };
#if 0
#ifdef INT3_DURING_FUNC_SKIP
            p->INT3_DURING_FUNC_SKIP_used[bp_no]=true;
            //L ("ret_adr=0x" PRI_ADR_HEX "\n", ret_adr);
            p->INT3_DURING_FUNC_SKIP_addresses[bp_no]=ret_adr;
            bool b=MC_ReadByte (mc, ret_adr, &p->INT3_DURING_FUNC_SKIP_byte[bp_no]);
            oassert(b && "can't read byte at breakpoint start");
            b=MC_WriteByte (mc, ret_adr, 0xCC);
            oassert(b && "can't write 0xCC byte at breakpoint start");

            if (verbose>0)
            {
                strbuf sb=STRBUF_INIT;
                process_get_sym (p, PC, /* add_module_name */ true, /* add_offset */ true, &sb);
                L ("(%d) Using INT3 to skip execution of %s\n", bp_no, sb.buf);
                strbuf_deinit(&sb);
            };
#else
            //clear_TF(ctx);
            CONTEXT_setDRx_and_DR7 (ctx, bp_no, ret_adr);
#endif
#endif
            BPF_skip_func (p, bp_no, ctx, mc, ret_adr);

            struct BP_thread_specific_dynamic_info *di=&t->BP_dynamic_info[bp_no];
            di->tracing_CALLs_executed--;
            if (verbose>0)
                L ("0x" PRI_ADR_HEX " skipping function, decreasing tracing_CALLs_executed, now it is %d\n", 
                        PC, di->tracing_CALLs_executed);

            return true;
        };
    };
    return false;
};

static bool handle_tracing_disassemble_and_cc (struct process *p, struct thread *t, struct MemoryCache *mc,
        CONTEXT *ctx, unsigned bp_no, struct Da* da)
{
    struct BP *bp=breakpoints[bp_no];
    struct BPF *bpf=bp->u.bpf;
    struct BP_thread_specific_dynamic_info *di=&t->BP_dynamic_info[bp_no];
    REG PC=CONTEXT_get_PC(ctx);
    bool disassembled=MC_disas(PC, mc, da);

    if (disassembled==false)
    {
        strbuf sb=STRBUF_INIT;
        process_get_sym(p, PC, true, true, &sb);

        L_once ("%s() disassemble failed for PC=%s (0x" PRI_ADR_HEX ")\n", __func__, sb.buf, PC);
        strbuf_deinit (&sb);
    }

    if (disassembled && da->ins_code==I_CALL)
    {
        if (limit_trace_nestedness && (di->tracing_CALLs_executed+1 >= limit_trace_nestedness))
        {
            // this call is to be skipped!
            if (verbose>0)
                L ("t->tracing_CALLs_executed=%d, limit_trace_nestedness=%d, skipping this CALL!\n",
                        di->tracing_CALLs_executed, limit_trace_nestedness);
            address new_adr=CONTEXT_get_PC(ctx) + da->ins_len;

            //CONTEXT_setDRx_and_DR7 (ctx, bp_no, new_adr);
            BPF_skip_func (p, bp_no, ctx, mc, new_adr);

            if (bpf->cc)                            // redundant?
                handle_cc(da, p, t, ctx, mc, false, true); // redundant?
            return true;
        }
        else
        {
            di->tracing_CALLs_executed++;
            if (verbose>0)
                L ("0x" PRI_ADR_HEX " CALL, increasing tracing_CALLs_executed, now it is %d\n", 
                        PC, di->tracing_CALLs_executed);
        };
    };

    if (disassembled && da->ins_code==I_RETN && di->tracing_CALLs_executed>0)
    {
        di->tracing_CALLs_executed--;
        if (verbose>0)
            L ("0x" PRI_ADR_HEX " RETN, decreasing tracing_CALLs_executed, now it is %d\n", 
                    PC, di->tracing_CALLs_executed);
    };

    if (bpf->cc)
        handle_cc(da, p, t, ctx, mc, false, false);
        
    return false;
};

enum ht_result
{
    ht_go_on,
    ht_finished,
    ht_need_to_skip_something
};

static void check_emulator_results_if_need (struct process *p, struct thread *t, CONTEXT *ctx)
{
    bool fail=false;

    if (t->last_emulated_present==false)
        return;

    bool last_emulated_ins_traced_by_one_step=ins_traced_by_one_step(t->last_emulated_ins->ins_code);

    // ... if last command was REP.... then check only if EIPs are different
    if (last_emulated_ins_traced_by_one_step && CONTEXT_get_PC(ctx)!=CONTEXT_get_PC(t->last_emulated_ctx))
        return;

    if (CONTEXT_compare (&cur_fds, ctx, t->last_emulated_ctx)==false)
    {
        L ("%s() (CPU emulator testing): CONTEXTs are different\n", __func__);
        L ("real context (ctx1):\n");
        dump_CONTEXT (&cur_fds, ctx, false, false, false);
        L ("emulated context (ctx2):\n");
        dump_CONTEXT (&cur_fds, t->last_emulated_ctx, false, false, false);
        fail=true;
    };

    if (MC_CompareInternalStateWithMemory(t->last_emulated_MC)==false)
    {
        L ("%s() (CPU emulator testing): memory states are different\n", __func__);
        fail=true;
    };

    if (fail)
    {
        address PC=CONTEXT_get_PC(ctx);
        strbuf sb=STRBUF_INIT;
        process_get_sym(p, PC, true, true, &sb);

        L ("%s() PC=%s (0x%x)\n", __func__, sb.buf, PC);
        strbuf_deinit (&sb);
        L ("last_emulated instruction="); Da_DumpString(&cur_fds, t->last_emulated_ins); L ("\n");
        exit(0);
    };
    
    if (verbose>0)
    {
        L ("last checked instruction="); Da_DumpString(&cur_fds, t->last_emulated_ins); L ("\n");
    };

    t->last_emulated_present=false;
    DFREE(t->last_emulated_ctx);
    t->last_emulated_ctx=NULL;
    MC_MemoryCache_dtor(t->last_emulated_MC, false);
    t->last_emulated_MC=NULL;
    DFREE(t->last_emulated_ins);
};

static bool emulate_if_need(struct process *p, struct thread *t, struct Da* da, CONTEXT *ctx, struct MemoryCache *mc)
{
    // TODO command-line option:
    //if (disable_emulator)
    //    return false;

    // if we test emulator, emulate next instruction only in one case: 
    // last_emulated_* were checked before by emulator tester and t->last_emulated_present is false
    // this is so for REP MOVSx/STOSx instructions testing
    if (emulator_testing && t->last_emulated_present)
        return false;

    if (da->ins_code==I_INVALID)
        return false; // can't emulate

    CONTEXT *new_ctx;
    struct MemoryCache *new_mc;

    if (emulator_testing)
    {
        new_ctx=DMEMDUP(ctx, sizeof(CONTEXT), "CONTEXT");
        new_mc=MC_MemoryCache_copy_ctor(mc);
    };

    if (verbose>1)
    {
        L ("instruction to be emulated="); Da_DumpString(&cur_fds, da); L ("\n");
    };

    enum Da_emulate_result r;

    if (emulator_testing)
        r=Da_emulate(da, new_ctx, new_mc, true, t->TIB);
    else
        r=Da_emulate(da, ctx, mc, true, t->TIB);

    if (r==DA_EMULATED_OK)
    {
        p->ins_emulated++;
        if ((p->ins_emulated&0xffff)==0)
            L ("Heartbeat: p->ins_emulated=%I64d\n", p->ins_emulated);
    }
    else
    {
        if (verbose>0)
        {
            strbuf tmp=STRBUF_INIT;
            Da_ToString(da, &tmp);
            L_once ("instruction wasn't emulated=%s\n", tmp.buf);
            strbuf_deinit(&tmp);
            p->ins_not_emulated++;
        };
        if (emulator_testing)
        {
            DFREE(new_ctx);
            MC_MemoryCache_dtor(new_mc, false);
            t->last_emulated_present=false;
        };
        return false;
    };

    if (verbose>1)
    {
        L ("instruction emulated successfully="); Da_DumpString(&cur_fds, da); L ("\n");
    };
    if (emulator_testing)
    {
        oassert(t->last_emulated_present==false);
        oassert(t->last_emulated_ctx==NULL);
        oassert(t->last_emulated_MC==NULL);
        t->last_emulated_ins=DMEMDUP (da, sizeof(struct Da), "struct Da");
        t->last_emulated_present=true;
        t->last_emulated_ctx=new_ctx;
        t->last_emulated_MC=new_mc;
    };
    return true;
};

static enum ht_result handle_tracing(int bp_no, struct process *p, struct thread *t, CONTEXT *ctx, struct MemoryCache *mc)
{
    address PC=CONTEXT_get_PC(ctx);
    struct BP_thread_specific_dynamic_info *di=&t->BP_dynamic_info[bp_no];

    if (verbose>1)
    {
        strbuf sb=STRBUF_INIT;
        process_get_sym(p, PC, true, true, &sb);

        L ("%s() PC=%s (0x%x)\n", __func__, sb.buf, PC);
        strbuf_deinit (&sb);
        L ("%s() di->ret_adr=0x" PRI_ADR_HEX ", di->SP_at_ret_adr=0x" PRI_ADR_HEX "\n",
                __func__, di->ret_adr, di->SP_at_ret_adr);
    };

    if (emulator_testing)
        check_emulator_results_if_need(p, t, ctx);
    
    bool emulated;
    do // emu cycle
    {
        emulated=false;
        PC=CONTEXT_get_PC(ctx);
        address SP=CONTEXT_get_SP(ctx);
        if (verbose>1)
            L ("%s() emu cycle begin. PC=0x" PRI_ADR_HEX " SP=0x" PRI_ADR_HEX "\n", __func__, PC, SP);
        // (to be implemented in future): check other BPF/BPX breakpoints. handle them if need.

        // finished?
        // check SP as well, this we need for tracing recursive functions correctly
        if (PC==di->ret_adr && (SP >= (di->SP_at_ret_adr)+sizeof(REG)))
            return ht_finished;

        struct Da da;

        // need to skip something? are we at the start of function to skip? is SYSCALL here? depth-level reached?
        // note: short-curcuit here
        if (handle_tracing_should_be_skipped (p, t, mc, ctx, bp_no) ||
            handle_tracing_disassemble_and_cc(p, t, mc, ctx, bp_no, &da))
        {
            t->last_emulated_present=false;
            return ht_need_to_skip_something;
        };

        emulated=emulate_if_need(p, t, &da, ctx, mc);

        if (emulator_testing)
            break;

    } while (emulated);

    // continue?
    return ht_go_on;
};

void handle_BPF(struct process *p, struct thread *t, int bp_no, CONTEXT *ctx, struct MemoryCache *mc)
{
    if (verbose>0)
        L ("%s() begin. PC=0x" PRI_ADR_HEX "\n", __func__, CONTEXT_get_PC (ctx));
    struct BP *bp=breakpoints[bp_no];
    struct BP_thread_specific_dynamic_info *di=&t->BP_dynamic_info[bp_no];
    enum BPF_state* state=&di->BPF_states;
    struct BPF *bpf=bp->u.bpf;
    unsigned u;

    switch (*state)
    {
        case BPF_state_default:

            u=handle_begin(p, t, bp, bp_no, ctx, mc);
            if (u==1)
                goto switch_to_default;
            if (u==2)
                goto handle_finish_and_switch_to_default;

            if (bpf->trace)
            {
                // begin tracing
                //set_TF(ctx);
                *state=BPF_state_tracing_inside_function;
                di->tracing=true;
                goto call_handle_tracing_etc;
            }
            else
                *state=BPF_state_at_return;
            break;

        case BPF_state_at_return:
            goto handle_finish_and_switch_to_default;

        case BPF_state_tracing_inside_function:
            goto call_handle_tracing_etc;

        case BPF_state_tracing_skipping:
            CONTEXT_setDRx_and_DR7 (ctx, bp_no, di->ret_adr); // return DRx back
            *state=BPF_state_tracing_inside_function;
            goto call_handle_tracing_etc;

        default:
            oassert(!"invalid *state");
    };
    goto exit;

call_handle_tracing_etc:
    switch (handle_tracing(bp_no, p, t, ctx, mc))
    {
        case ht_go_on: // go on
            if (verbose>0)
                L ("handle_tracing() -> ht_go_on. PC=0x" PRI_ADR_HEX "\n", CONTEXT_get_PC (ctx));
            di->tracing=true; 
            break;
        case ht_finished: // finished
            if (verbose>0)
                L ("handle_tracing() -> ht_finished\n");
            di->tracing=false;
            REMOVE_BIT(ctx->Dr6, FLAG_DR6_BS);
            goto handle_finish_and_switch_to_default;
        case ht_need_to_skip_something: // need to skip something
            if (verbose>0)
                L ("handle_tracing() -> ht_need_to_skip_something\n");
            di->tracing=false;
            *state=BPF_state_tracing_skipping;
            break;
    };

    if (di->tracing)
        set_TF(ctx);
    else
        clear_TF(ctx);

    goto exit;

handle_finish_and_switch_to_default:
    handle_finish(p, t, bp, bp_no, ctx, mc);
switch_to_default:
    *state=BPF_state_default;

exit:
    if (verbose>0)
        L ("%s() end. TF=%s, PC=0x" PRI_ADR_HEX "\n", 
                __func__, bool_to_string(IS_SET(ctx->EFlags, FLAG_TF)), CONTEXT_get_PC (ctx));
};

void handle_BPF_INT3(struct process *p, struct thread *t, int DRx_no, CONTEXT *ctx, struct MemoryCache *mc)
{
    if (verbose>0)
        L ("%s() begin. PC=0x" PRI_ADR_HEX "\n", __func__, CONTEXT_get_PC (ctx));

    p->INT3_DURING_FUNC_SKIP_used[DRx_no]=false;
    bool b=MC_WriteByte (mc, p->INT3_DURING_FUNC_SKIP_addresses[DRx_no], p->INT3_DURING_FUNC_SKIP_byte[DRx_no]);
    if (b==false)
    {
        L ("can't restore byte at 0x" PRI_ADR_HEX ", byte=0x%02X\n", p->INT3_DURING_FUNC_SKIP_addresses[DRx_no], p->INT3_DURING_FUNC_SKIP_byte[DRx_no]);
        exit(0);
    };
    CONTEXT_decrement_PC (ctx);

    handle_BPF(p, t, DRx_no, ctx, mc);

    if (verbose>0)
        L ("%s() end. \n", __func__);
};

/* vim: set expandtab ts=4 sw=4 : */
