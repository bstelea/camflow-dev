/*
*
* /linux/security/provenance/hooks.c
*
* Author: Thomas Pasquier <tfjmp2@cam.ac.uk>
*
* Copyright (C) 2015 University of Cambridge
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2, as
* published by the Free Software Foundation.
*
*/

#include <linux/provenance.h>
#include <linux/slab.h>
#include <linux/lsm_hooks.h>
#include <linux/msg.h>

atomic64_t prov_node_id=ATOMIC64_INIT(0);
struct kmem_cache *provenance_cache=NULL;
struct kmem_cache *long_provenance_cache=NULL;

static inline prov_msg_t* alloc_provenance(node_id_t nid, message_type_t ntype, gfp_t gfp)
{
  prov_msg_t* prov =  kmem_cache_zalloc(provenance_cache, gfp);
  if(!prov){
    return NULL;
  }

  if(nid==0)
  {
    prov->node_info.node_id=prov_next_nodeid();
  }else{
    prov->node_info.node_id=nid;
  }
  prov->node_info.message_type=ntype;
  return prov;
}

static inline void free_provenance(prov_msg_t* prov){
  kmem_cache_free(provenance_cache, prov);
}

static inline prov_msg_t* provenance_clone(message_type_t ntype, prov_msg_t* old, gfp_t gfp)
{
  prov_msg_t* prov =   alloc_provenance(0, ntype, gfp);
  if(!prov)
  {
    return NULL;
  }
  return prov;
}


/*
 * initialise the security for the init task
 */
static void cred_init_provenance(void)
{
	struct cred *cred = (struct cred *) current->real_cred;
	prov_msg_t *prov;

	prov = alloc_provenance(0, MSG_TASK, GFP_KERNEL);
	if (!prov)
		panic("Provenance:  Failed to initialize initial task.\n");
  prov->node_info.uid=__kuid_val(cred->euid);
  prov->node_info.gid=__kgid_val(cred->egid);

	cred->provenance = prov;
}

/*
* @cred points to the credentials.
* @gfp indicates the atomicity of any memory allocations.
* Only allocate sufficient memory and attach to @cred such that
* cred_transfer() will not get ENOMEM.
*/
static int provenance_cred_alloc_blank(struct cred *cred, gfp_t gfp)
{
  prov_msg_t* prov;

  prov = alloc_provenance(0, MSG_TASK, gfp);

  if(!prov)
    return -ENOMEM;
  prov->node_info.uid=__kuid_val(cred->euid);
  prov->node_info.gid=__kgid_val(cred->egid);

  cred->provenance = prov;
  return 0;
}

/*
* @cred points to the credentials.
* Deallocate and clear the cred->security field in a set of credentials.
*/
static void provenance_cred_free(struct cred *cred)
{
  free_provenance(cred->provenance);
  cred->provenance = NULL;
}

/*
* @new points to the new credentials.
* @old points to the original credentials.
* @gfp indicates the atomicity of any memory allocations.
* Prepare a new set of credentials by copying the data from the old set.
*/
static int provenance_cred_prepare(struct cred *new, const struct cred *old, gfp_t gfp)
{
  prov_msg_t* old_prov = old->provenance;
  prov_msg_t* prov = provenance_clone(MSG_TASK, old_prov, gfp);

  if(!prov)
    return -ENOMEM;
  prov->node_info.uid=__kuid_val(new->euid);
  prov->node_info.gid=__kgid_val(new->egid);

  if(old_prov->node_info.tracked == NODE_TRACKED || prov_all)
  {
    record_edge(ED_CREATE, old_prov, prov);
  }

  new->provenance = prov;
  return 0;
}

/*
* @new points to the new credentials.
* @old points to the original credentials.
* Transfer data from original creds to new creds
*/
static void provenance_cred_transfer(struct cred *new, const struct cred *old)
{
  const prov_msg_t *old_prov = old->provenance;
	prov_msg_t *prov = new->provenance;

  *prov=*old_prov;
}

/*
* Update the module's state after setting one or more of the user
* identity attributes of the current process.  The @flags parameter
* indicates which of the set*uid system calls invoked this hook.  If
* @new is the set of credentials that will be installed.  Modifications
* should be made to this rather than to @current->cred.
* @old is the set of credentials that are being replaces
* @flags contains one of the LSM_SETID_* values.
* Return 0 on success.
*/
static int provenance_task_fix_setuid(struct cred *new, const struct cred *old, int flags)
{
  prov_msg_t *old_prov = old->provenance;
	prov_msg_t *prov = new->provenance;

  if(old_prov->node_info.tracked == NODE_TRACKED || prov->node_info.tracked == NODE_TRACKED || prov_all) // record if entity tracked or if record everyting
  {
    record_edge(ED_CHANGE, old_prov, prov);
  }

  return 0;
}

/*
* Allocate and attach a security structure to @inode->i_security.  The
* i_security field is initialized to NULL when the inode structure is
* allocated.
* @inode contains the inode structure.
* Return 0 if operation was successful.
*/
static int provenance_inode_alloc_security(struct inode *inode)
{
  prov_msg_t* cprov = current_provenance();
  prov_msg_t* iprov;
  iprov = alloc_provenance(inode->i_ino, MSG_INODE, GFP_NOFS);
  if(unlikely(!iprov))
    return -ENOMEM;
  iprov->inode_info.uid=__kuid_val(inode->i_uid);
  iprov->inode_info.gid=__kgid_val(inode->i_gid);
  iprov->inode_info.mode=inode->i_mode;
  iprov->inode_info.rdev=inode->i_rdev;

  inode->i_provenance = iprov;

  if(cprov->node_info.tracked==NODE_TRACKED || iprov->node_info.tracked==NODE_TRACKED || prov_all){
    record_edge(ED_CREATE, cprov, iprov); /* creating inode != creating the file */
  }
  return 0;
}

/*
* @inode contains the inode structure.
* Deallocate the inode security structure and set @inode->i_security to
* NULL.
*/
static void provenance_inode_free_security(struct inode *inode)
{
  prov_msg_t* prov = inode->i_provenance;
  inode->i_provenance=NULL;
  if(!prov)
    free_provenance(prov);
}

/*
* Check permission before accessing an inode.  This hook is called by the
* existing Linux permission function, so a security module can use it to
* provide additional checking for existing Linux permission checks.
* Notice that this hook is called when a file is opened (as well as many
* other operations), whereas the file_security_ops permission hook is
* called when the actual read/write operations are performed.
* @inode contains the inode structure to check.
* @mask contains the permission mask.
* Return 0 if permission is granted.
*/
static int provenance_inode_permission(struct inode *inode, int mask)
{
  prov_msg_t* cprov = current_provenance();
  prov_msg_t* iprov;

  if(!inode->i_provenance){ // alloc provenance if none there
    provenance_inode_alloc_security(inode);
  }

  iprov = inode->i_provenance;

  mask &= (MAY_READ|MAY_WRITE|MAY_EXEC|MAY_APPEND);

  if((mask & (MAY_WRITE|MAY_APPEND)) != 0){
    if(cprov->node_info.tracked==NODE_TRACKED || iprov->node_info.tracked==NODE_TRACKED || prov_all){
      record_edge(ED_DATA, cprov, iprov);
    }
  }
  if((mask & (MAY_READ|MAY_EXEC|MAY_WRITE|MAY_APPEND)) != 0){
    // conservatively assume write imply read
    if(cprov->node_info.tracked==NODE_TRACKED || iprov->node_info.tracked==NODE_TRACKED || prov_all){
      record_edge(ED_DATA, iprov, cprov);
    }
  }
  return 0;
}

/*
* Check permission before creating a new hard link to a file.
* @old_dentry contains the dentry structure for an existing
* link to the file.
* @dir contains the inode structure of the parent directory
* of the new link.
* @new_dentry contains the dentry structure for the new link.
* Return 0 if permission is granted.
*/

static int provenance_inode_link(struct dentry *old_dentry, struct inode *dir, struct dentry *new_dentry)
{
  prov_msg_t* cprov = current_provenance();
  prov_msg_t* dprov;
  prov_msg_t* iprov;
  long_prov_msg_t* link_prov;

  if(!dir->i_provenance){ // alloc provenance if none there
    provenance_inode_alloc_security(dir);
  }

  if(!old_dentry->d_inode->i_provenance){
    provenance_inode_alloc_security(old_dentry->d_inode);
  }

  dprov = dir->i_provenance; // directory
  iprov = old_dentry->d_inode->i_provenance; // inode pointed by dentry

  // writing to the directory
  if(cprov->node_info.tracked==NODE_TRACKED
    || dprov->node_info.tracked==NODE_TRACKED
    || iprov->node_info.tracked==NODE_TRACKED
    || prov_all){
    record_edge(ED_DATA, cprov, dprov);
    record_edge(ED_DATA, cprov, iprov);
  }

  if(prov_enabled){
    link_prov = kmem_cache_zalloc(long_provenance_cache, GFP_NOFS);
    link_prov->link_info.message_type = MSG_LINK;
    link_prov->link_info.length = new_dentry->d_name.len;
    memcpy(link_prov->link_info.name, new_dentry->d_name.name, link_prov->link_info.length);
    link_prov->link_info.dir_id = dprov->inode_info.node_id;
    link_prov->link_info.task_id = cprov->task_info.node_id;
    link_prov->link_info.inode_id = iprov->task_info.node_id;
    long_prov_write(link_prov);
    kmem_cache_free(long_provenance_cache, link_prov);
  }
  return 0;
}

/*
* Check the permission to remove a hard link to a file.
* @dir contains the inode structure of parent directory of the file.
* @dentry contains the dentry structure for file to be unlinked.
* Return 0 if permission is granted.
*/
static int provenance_inode_unlink(struct inode *dir, struct dentry *dentry)
{
  prov_msg_t* cprov = current_provenance();
  prov_msg_t* dprov;
  prov_msg_t* iprov;
  long_prov_msg_t* link_prov;

  if(!dir->i_provenance){ // alloc provenance if none there
    provenance_inode_alloc_security(dir);
  }

  if(!dentry->d_inode->i_provenance){
    provenance_inode_alloc_security(dentry->d_inode);
  }

  dprov = dir->i_provenance; // directory
  iprov = dentry->d_inode->i_provenance; // inode pointed by dentry

  // writing to the directory
  if(cprov->node_info.tracked==NODE_TRACKED
    || dprov->node_info.tracked==NODE_TRACKED
    || iprov->node_info.tracked==NODE_TRACKED
    || prov_all){
    record_edge(ED_DATA, cprov, dprov);
    record_edge(ED_DATA, cprov, iprov);
  }

  if(prov_enabled){
    link_prov = kmem_cache_zalloc(long_provenance_cache, GFP_NOFS);
    link_prov->link_info.message_type = MSG_UNLINK;
    link_prov->unlink_info.length = dentry->d_name.len;
    memcpy(link_prov->unlink_info.name, dentry->d_name.name, link_prov->unlink_info.length);
    link_prov->unlink_info.dir_id = dprov->inode_info.node_id;
    link_prov->unlink_info.task_id = cprov->task_info.node_id;
    link_prov->unlink_info.inode_id = iprov->task_info.node_id;
    long_prov_write(link_prov);
    kmem_cache_free(long_provenance_cache, link_prov);
  }
  return 0;
}

/*
* Check file permissions before accessing an open file.  This hook is
* called by various operations that read or write files.  A security
* module can use this hook to perform additional checking on these
* operations, e.g.  to revalidate permissions on use to support privilege
* bracketing or policy changes.  Notice that this hook is used when the
* actual read/write operations are performed, whereas the
* inode_security_ops hook is called when a file is opened (as well as
* many other operations).
* Caveat:  Although this hook can be used to revalidate permissions for
* various system call operations that read or write files, it does not
* address the revalidation of permissions for memory-mapped files.
* Security modules must handle this separately if they need such
* revalidation.
* @file contains the file structure being accessed.
* @mask contains the requested permissions.
* Return 0 if permission is granted.
*/
static int provenance_file_permission(struct file *file, int mask)
{
  struct inode *inode = file_inode(file);
  if(!inode->i_provenance){ // alloc provenance if none there
    provenance_inode_alloc_security(inode);
  }
  provenance_inode_permission(inode, mask);
  return 0;
}

/*
* Check permissions for a mmap operation.  The @file may be NULL, e.g.
* if mapping anonymous memory.
* @file contains the file structure for file to map (may be NULL).
* @reqprot contains the protection requested by the application.
* @prot contains the protection that will be applied by the kernel.
* @flags contains the operational flags.
* Return 0 if permission is granted.
*/
static int provenance_mmap_file(struct file *file, unsigned long reqprot, unsigned long prot, unsigned long flags)
{
  prov_msg_t* cprov = current_provenance();
  prov_msg_t* iprov;
  struct inode *inode;
  if(file==NULL){ // what to do for NULL?
    return 0;
  }
  inode = file_inode(file);
  if(!inode->i_provenance){ // alloc provenance if none there
    provenance_inode_alloc_security(inode);
  }
  iprov = inode->i_provenance;
  prot &= (PROT_EXEC|PROT_READ|PROT_WRITE);

  if((prot & (PROT_WRITE|PROT_EXEC)) != 0){
    if(cprov->node_info.tracked==NODE_TRACKED || iprov->node_info.tracked==NODE_TRACKED || prov_all){
      record_edge(ED_MMAP, cprov, iprov);
    }
  }
  if((prot & (PROT_READ|PROT_EXEC|PROT_WRITE)) != 0){
    // conservatively assume write imply read
    if(cprov->node_info.tracked==NODE_TRACKED || iprov->node_info.tracked==NODE_TRACKED || prov_all){
      record_edge(ED_MMAP, iprov, cprov);
    }
  }
  return 0;
}

/*
* @file contains the file structure.
* @cmd contains the operation to perform.
* @arg contains the operational arguments.
* Check permission for an ioctl operation on @file.  Note that @arg
* sometimes represents a user space pointer; in other cases, it may be a
* simple integer value.  When @arg represents a user space pointer, it
* should never be used by the security module.
* Return 0 if permission is granted.
*/
static int provenance_file_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
  prov_msg_t* cprov = current_provenance();
  prov_msg_t* iprov;
  struct inode *inode = file_inode(file);

  if(!inode->i_provenance){ // alloc provenance if none there
    provenance_inode_alloc_security(inode);
  }
  iprov = inode->i_provenance;
  if(cprov->node_info.tracked==NODE_TRACKED || iprov->node_info.tracked==NODE_TRACKED || prov_all){
    record_edge(ED_DATA, iprov, cprov); // both way exchange
    record_edge(ED_DATA, cprov, iprov);
  }

  return 0;
}

/* msg */

/*
* Allocate and attach a security structure to the msg->security field.
* The security field is initialized to NULL when the structure is first
* created.
* @msg contains the message structure to be modified.
* Return 0 if operation was successful and permission is granted.
*/
static int provenance_msg_msg_alloc_security(struct msg_msg *msg)
{
  prov_msg_t* cprov = current_provenance();
  prov_msg_t* mprov;
  /* alloc new prov struct with generated id */
  mprov = alloc_provenance(0, MSG_MSG, GFP_NOFS);

  if(!mprov)
    return -ENOMEM;

  mprov->msg_msg_info.type=msg->m_type;
  msg->provenance = mprov;
  if(cprov->node_info.tracked==NODE_TRACKED || mprov->node_info.tracked==NODE_TRACKED || prov_all){
    record_edge(ED_CREATE, cprov, mprov);
  }
  return 0;
}

/*
* Deallocate the security structure for this message.
* @msg contains the message structure to be modified.
*/
static void provenance_msg_msg_free_security(struct msg_msg *msg)
{
  free_provenance(msg->provenance);
  msg->provenance=NULL;
}

/*
* Check permission before a message, @msg, is enqueued on the message
* queue, @msq.
* @msq contains the message queue to send message to.
* @msg contains the message to be enqueued.
* @msqflg contains operational flags.
* Return 0 if permission is granted.
*/
static int provenance_msg_queue_msgsnd(struct msg_queue *msq, struct msg_msg *msg, int msqflg)
{
  prov_msg_t* cprov = current_provenance();
  prov_msg_t* mprov = msg->provenance;
  if(cprov->node_info.tracked==NODE_TRACKED || mprov->node_info.tracked==NODE_TRACKED || prov_all){
    record_edge(ED_DATA, cprov, mprov);
  }
  return 0;
}
/*
* Check permission before a message, @msg, is removed from the message
* queue, @msq.  The @target task structure contains a pointer to the
* process that will be receiving the message (not equal to the current
* process when inline receives are being performed).
* @msq contains the message queue to retrieve message from.
* @msg contains the message destination.
* @target contains the task structure for recipient process.
* @type contains the type of message requested.
* @mode contains the operational flags.
* Return 0 if permission is granted.
*/
static int provenance_msg_queue_msgrcv(struct msg_queue *msq, struct msg_msg *msg,
				    struct task_struct *target,
				    long type, int mode)
{
  prov_msg_t* cprov = target->cred->provenance;
  prov_msg_t* mprov = msg->provenance;

  if(cprov->node_info.tracked==NODE_TRACKED || mprov->node_info.tracked==NODE_TRACKED || prov_all){
    record_edge(ED_DATA, mprov, cprov);
  }
  return 0;
}

static struct security_hook_list provenance_hooks[] = {
  LSM_HOOK_INIT(cred_alloc_blank, provenance_cred_alloc_blank),
  LSM_HOOK_INIT(cred_free, provenance_cred_free),
  LSM_HOOK_INIT(cred_prepare, provenance_cred_prepare),
  LSM_HOOK_INIT(cred_transfer, provenance_cred_transfer),
  LSM_HOOK_INIT(task_fix_setuid, provenance_task_fix_setuid),
  LSM_HOOK_INIT(inode_alloc_security, provenance_inode_alloc_security),
  LSM_HOOK_INIT(inode_free_security, provenance_inode_free_security),
  LSM_HOOK_INIT(inode_permission, provenance_inode_permission),
  LSM_HOOK_INIT(file_permission, provenance_file_permission),
  LSM_HOOK_INIT(mmap_file, provenance_mmap_file),
  LSM_HOOK_INIT(file_ioctl, provenance_file_ioctl),
  LSM_HOOK_INIT(inode_link, provenance_inode_link),
	LSM_HOOK_INIT(inode_unlink, provenance_inode_unlink),
	LSM_HOOK_INIT(msg_msg_alloc_security, provenance_msg_msg_alloc_security),
	LSM_HOOK_INIT(msg_msg_free_security, provenance_msg_msg_free_security),
  LSM_HOOK_INIT(msg_queue_msgsnd, provenance_msg_queue_msgsnd),
  LSM_HOOK_INIT(msg_queue_msgrcv, provenance_msg_queue_msgrcv),
};

void __init provenance_add_hooks(void){
  provenance_cache = kmem_cache_create("provenance_struct",
					    sizeof(prov_msg_t),
					    0, SLAB_PANIC, NULL);
  long_provenance_cache = kmem_cache_create("long_provenance_struct",
					    sizeof(long_prov_msg_t),
					    0, SLAB_PANIC, NULL);
  cred_init_provenance();
  /* register the provenance security hooks */
  security_add_hooks(provenance_hooks, ARRAY_SIZE(provenance_hooks));
}