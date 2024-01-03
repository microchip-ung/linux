/* Copyright (c) 2015 Microsemi Corporation

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/

#include <linux/inetdevice.h>
#include <net/genetlink.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "vtss_dying_gasp.h"

/* fwd */
static struct genl_family vtss_dying_gasp_genl_family;
/* mutex for dying gasp */
static DEFINE_MUTEX(vtss_dying_gasp_genl_sem);
/* link list for dying gasp */
struct list_head VTSS_DYING_GASP_GENL_BUF;
/* trivial proc file for debug */
static struct proc_dir_entry *proc_dying_gasp = 0;

/* dying gasp attibutes */
enum vtss_dying_gasp_attr {
    VTSS_DYING_GASP_ATTR_NONE,
    VTSS_DYING_GASP_ATTR_ID,
    VTSS_DYING_GASP_ATTR_INTERFACE,
    VTSS_DYING_GASP_ATTR_MSG,

    /* Add new entries here, and please remember to update the user-space
     * applications */
    VTSS_DYING_GASP_ATTR_END,
};

#define VTSS_DYING_GASP_ATTR_MAX VTSS_DYING_GASP_ATTR_END - 1

/* dying gasp methods */
enum vtss_dying_gasp_genl {
    VTSS_DYING_GASP_GENL_BUF_ADD,
    VTSS_DYING_GASP_GENL_BUF_MODIFY,
    VTSS_DYING_GASP_GENL_BUF_DELETE,
    VTSS_DYING_GASP_GENL_BUF_DELETE_ALL,
    /* Add new entries here, and please remember to update the user-space
     * applications */
};

/* dying gasp helper functions */
static struct vtss_dying_gasp_genl_buf *find_buf_by_id(int id)
{
    struct vtss_dying_gasp_genl_buf *res = NULL, *r;

    rcu_read_lock();
    list_for_each_entry (r, &VTSS_DYING_GASP_GENL_BUF, list) {
        if (r->id == id) {
            BUG_ON(res);
            res = r;
        }
    }
    rcu_read_unlock();
    return res;
}

static void buf_free(struct rcu_head *head)
{
    struct vtss_dying_gasp_genl_buf *b =
            container_of(head, struct vtss_dying_gasp_genl_buf, rcu);
    kfree(b);
}

static struct vtss_dying_gasp_genl_buf *parse_buf(struct sk_buff *skb,
                                                  struct genl_info *info,
                                                  int id, int *err)
{
    struct vtss_dying_gasp_genl_buf *b = NULL;

    if (info->attrs[VTSS_DYING_GASP_ATTR_MSG]) {
        b = (struct vtss_dying_gasp_genl_buf *)
                kmalloc(sizeof(struct vtss_dying_gasp_genl_buf) +
                    nla_len(info->attrs[VTSS_DYING_GASP_ATTR_MSG]),
                    GFP_KERNEL | __GFP_ZERO);
        if (!b) {
            printk(KERN_ERR "kmalloc for dying gasp buf failed!\n");
            *err = -ENOMEM;
            return NULL;
        }

        b->id = id;
        b->msg_len = nla_len(info->attrs[VTSS_DYING_GASP_ATTR_MSG]);
        nla_memcpy(b->msg, info->attrs[VTSS_DYING_GASP_ATTR_MSG], b->msg_len);
    } else {
        printk(KERN_ERR "Invalid dying gasp msg!\n");
        *err = -EINVAL;
        return NULL;
    }
    /* ready to return the filled dying_gasp_buf struct */
    return b;
}

static int get_free_id(void)
{

    static u32 last_id = 0;
    struct vtss_dying_gasp_genl_buf *b;

    mutex_lock(&vtss_dying_gasp_genl_sem);
    rcu_read_lock();

AGAIN:
    last_id++;

    /* handle wrap around */
    if (last_id <= 0) {
        last_id = 0;
        goto AGAIN;
    }

    list_for_each_entry_rcu(b, &VTSS_DYING_GASP_GENL_BUF, list) {
        if (b->id == last_id)
            goto AGAIN;
    }

    rcu_read_unlock();
    mutex_unlock(&vtss_dying_gasp_genl_sem);

    return last_id;
};

static int vtss_dying_gasp_genl_buf_add(struct sk_buff *skb,
                                        struct genl_info *info)
{
    int id;  /* return a unique id back to user space */
    int err = -1;
    void *hdr;  /* generic netlink msg header */
    struct sk_buff *msg;  /* generic netlink msg */
    struct vtss_dying_gasp_genl_buf *b = NULL;

    /* sanity check */
    if (!info->attrs[VTSS_DYING_GASP_ATTR_INTERFACE]) {
        printk(KERN_ERR "INTERFACE is missing!\n");
        return -EINVAL;
    }
    if (!info->attrs[VTSS_DYING_GASP_ATTR_MSG]) {
        printk(KERN_ERR "MSG is missing!\n");
        return -EINVAL;
    }

    /* allocate a netlink msg buf */
    msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
    if (!msg) {
        printk(KERN_ERR "Allocate netlink msg failed!\n");
        return -ENOMEM;
    }

    /* create msg hdr */
    hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
                      &vtss_dying_gasp_genl_family, 0,
                      VTSS_DYING_GASP_GENL_BUF_ADD);

    if (!hdr) {
        printk(KERN_ERR "Create msg hdr failed!\n");
        err = -EMSGSIZE;
        goto ERROR_MEM_MSG;
    }

    /* generate a unique id */
    id = get_free_id();
    /* put id in msg back to user-space */
    if (nla_put_u32(msg, VTSS_DYING_GASP_ATTR_ID, id)) {
        printk(KERN_ERR "Add id attribute back failed!\n");
        err = -EMSGSIZE;
        goto ERROR_GENLMSG;
    }

    /* parse the dying gasp msg from user-space */
    b = parse_buf(skb, info, id, &err);
    if (!b)
        goto ERROR_GENLMSG;

    /* Install the dying gasp buf into the configured list */
    mutex_lock(&vtss_dying_gasp_genl_sem);
    list_add_rcu(&b->list, &VTSS_DYING_GASP_GENL_BUF);
    mutex_unlock(&vtss_dying_gasp_genl_sem);

    /* finalize msg */
    genlmsg_end(msg, hdr);

    /* Send msg */
    err = genlmsg_unicast(genl_info_net(info), msg, info->snd_portid);

    if (err != 0) {
        printk(KERN_ERR "Send msg failed!\n");
    }
    return err;

ERROR_GENLMSG:
    genlmsg_cancel(skb, hdr);

ERROR_MEM_MSG:
    nlmsg_free(msg);

    return err;
}

static int vtss_dying_gasp_genl_buf_modify(struct sk_buff *skb,
                                           struct genl_info *info)
{
    int id;
    int err = -1;
    struct vtss_dying_gasp_genl_buf *b_new = NULL, *b_old = NULL;

    if (!info->attrs[VTSS_DYING_GASP_ATTR_ID]) {
        printk(KERN_ERR "ID is missing!\n");
        return -EINVAL;
    }

    id = nla_get_u32(info->attrs[VTSS_DYING_GASP_ATTR_ID]);

    /* pass new msg from user space to kernel dying gasp buf */
    b_new = parse_buf(skb, info, id, &err);
    if (!b_new) {
        printk(KERN_ERR "%s:%d failed!\n", __FUNCTION__, __LINE__);
        /* err will be updated by parse_buf() */
        goto ERROR;
    }

    /* find the old kernel dying gasp buf by id */
    b_old = find_buf_by_id(id);
    if (!b_old) {
        printk(KERN_ERR "%s:%d failed!\n", __FUNCTION__, __LINE__);
        err = -ENOENT;
        goto ERROR;
    }

    /* if interface is not given */
    if (!info->attrs[VTSS_DYING_GASP_ATTR_INTERFACE])
        memcpy(b_new->interface, b_old->interface, 16);

    /* if msg is not given */
    if (!info->attrs[VTSS_DYING_GASP_ATTR_MSG]) {
        memcpy(b_new->msg, b_old->msg, b_new->msg_len);
    }

    mutex_lock(&vtss_dying_gasp_genl_sem);
    list_replace_rcu(&b_old->list, &b_new->list);
    /* queue an rcu callback for freeing the old entry after a grace period */
    call_rcu(&b_old->rcu, buf_free);
    mutex_unlock(&vtss_dying_gasp_genl_sem);

    return 0;

ERROR:
    kfree(b_new);
    return err;
}

static int vtss_dying_gasp_genl_buf_delete(struct sk_buff *skb,
                                           struct genl_info *info)
{
    u32 id;
    struct vtss_dying_gasp_genl_buf *b;
    int cnt = 0;

    if (info->attrs[VTSS_DYING_GASP_ATTR_ID]) {
        /* delete list entry by id */
        id = nla_get_u32(info->attrs[VTSS_DYING_GASP_ATTR_ID]);
        b = find_buf_by_id(id);
        if (!b) {
            printk(KERN_ERR "No match for id: %d!\n", id);
            return -ENOENT;
        }

        mutex_lock(&vtss_dying_gasp_genl_sem);
        list_del_rcu(&b->list);
        call_rcu(&b->rcu, buf_free);
        mutex_unlock(&vtss_dying_gasp_genl_sem);
    } else {
        /* delete all entries in the list, if id is not given */
        mutex_lock(&vtss_dying_gasp_genl_sem);
        list_for_each_entry(b, &VTSS_DYING_GASP_GENL_BUF, list) {
            cnt++;
            list_del_rcu(&b->list);
            call_rcu(&b->rcu, buf_free);
        }
        mutex_unlock(&vtss_dying_gasp_genl_sem);
        return cnt;
    }
    return 0;
}

static int vtss_dying_gasp_genl_buf_delete_all(struct sk_buff *skb,
                                               struct genl_info *info)
{
    struct vtss_dying_gasp_genl_buf *b;
    int cnt = 0;

    mutex_lock(&vtss_dying_gasp_genl_sem);
    list_for_each_entry(b, &VTSS_DYING_GASP_GENL_BUF, list) {
        cnt++;
        list_del_rcu(&b->list);
        call_rcu(&b->rcu, buf_free);
    }
    mutex_unlock(&vtss_dying_gasp_genl_sem);
    return 0;
}

/* dying gasp genl_ops */
static struct genl_ops vtss_dying_gasp_genl_ops[] = {
    {
        .cmd    = VTSS_DYING_GASP_GENL_BUF_ADD,
        .doit   = vtss_dying_gasp_genl_buf_add,
        .flags  = GENL_ADMIN_PERM,
    },
    {
        .cmd    = VTSS_DYING_GASP_GENL_BUF_MODIFY,
        .doit   = vtss_dying_gasp_genl_buf_modify,
        .flags  = GENL_ADMIN_PERM,
    },
    {
        .cmd    = VTSS_DYING_GASP_GENL_BUF_DELETE,
        .doit   = vtss_dying_gasp_genl_buf_delete,
        .flags  = GENL_ADMIN_PERM,
    },
    {
        .cmd    = VTSS_DYING_GASP_GENL_BUF_DELETE_ALL,
        .doit   = vtss_dying_gasp_genl_buf_delete_all,
        .flags  = GENL_ADMIN_PERM,
    },
};

/****************************************************************************/
// Policies
// Basically, this structure tells what kind of data comes with a given
// attribute.
/****************************************************************************/
static const struct nla_policy vtss_dying_gasp_genl_policy[VTSS_DYING_GASP_ATTR_END] = {
    [VTSS_DYING_GASP_ATTR_NONE]      = {.type = NLA_UNSPEC},
    [VTSS_DYING_GASP_ATTR_ID]        = {.type = NLA_U32},
    [VTSS_DYING_GASP_ATTR_INTERFACE] = {.type = NLA_STRING, .len =   16},
    [VTSS_DYING_GASP_ATTR_MSG]       = {.type = NLA_BINARY, .len = 4096},
};

/* dying gasp genl_family */
static struct genl_family vtss_dying_gasp_genl_family = {
    .hdrsize = 0,
    .name    = "vtss_dying_gasp",
    .version = 1,
    .maxattr = VTSS_DYING_GASP_ATTR_MAX,
    .policy  = vtss_dying_gasp_genl_policy,
    .ops     = vtss_dying_gasp_genl_ops,
    .n_ops   = ARRAY_SIZE(vtss_dying_gasp_genl_ops),
};

/* used by "cat /proc/vtss_dying_gasp" for debug */
static void debug_dump_dying_gasp(struct seq_file *s,
                                  struct vtss_dying_gasp_genl_buf *b)
{
    int i;
    seq_printf(s, "  ID: %d, IF: %s LEN: %d \n", b->id, b->interface,
               b->msg_len);


    for (i = 0; i < b->msg_len; ++i) {
        seq_printf(s, "%02hhx ", b->msg[i]);
        if ( (i + 1) % 16 == 0)
            seq_printf(s, "\n");
    }
    seq_printf(s, "\n");
}

static int debug_dump_(struct seq_file *s, void *v)
{
    struct vtss_dying_gasp_genl_buf *b;

    seq_printf(s, "DYING_GASP_BUF:\n");
    rcu_read_lock();
    /* STACK - last in first out, not FIFO */
    /* STACK or FIFO should be consistent with dying_gasp_isr */
    list_for_each_entry_rcu(b, &VTSS_DYING_GASP_GENL_BUF, list) {
        debug_dump_dying_gasp(s, b);
    }
    rcu_read_unlock();
    seq_printf(s, "END\n");

    return 0;
}

static int debug_dying_gasp(struct inode *inode, struct file *file)
{
    return single_open(file, debug_dump_, NULL);
}


static const struct proc_ops dying_gasp_fops = {
    .proc_open    = debug_dying_gasp,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/* dying gasp init func */
int vtss_dying_gasp_genetlink_init(void)
{
    int err;

    INIT_LIST_HEAD_RCU(&VTSS_DYING_GASP_GENL_BUF);

#if defined (CONFIG_PROC_FS)
    proc_dying_gasp = proc_create("vtss_dying_gasp", S_IRUGO, NULL, &dying_gasp_fops);
    if (!proc_dying_gasp)
        return -ENOMEM;
#endif  /* CONFIG_PROC_FS */

    err = genl_register_family(&vtss_dying_gasp_genl_family);
    if (err == -1) {
        printk(KERN_ERR "%s FAILED!\n", __FUNCTION__);
    }
    return err;
}

/* dying gasp uninit func */
void vtss_dying_gasp_genetlink_uninit(void)
{
#if defined (CONFIG_PROC_FS)
    if (proc_dying_gasp)
        proc_remove(proc_dying_gasp);
#endif  /* CONFIG_PROC_FS */

    genl_unregister_family(&vtss_dying_gasp_genl_family);
}
