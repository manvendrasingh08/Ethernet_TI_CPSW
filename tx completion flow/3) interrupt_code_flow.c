
irqreturn_t cpsw_tx_interrupt(int irq, void *dev_id)
{
	struct cpsw_common *cpsw = dev_id;

	writel(0, &cpsw->wr_regs->tx_en);   // disable the tx interrupts by putting 0 in tx_en register.  🛑️Complete flow in "4)" file.
	cpdma_ctlr_eoi(cpsw->dma, CPDMA_EOI_TX);  // it provides the eacknowledgement that the interrupt has been recieved to the source.

	if (cpsw->quirk_irq) {
		disable_irq_nosync(cpsw->irqs_table[1]);  //“Do not invoke the ISR for this IRQ anymore”
		cpsw->tx_irq_disabled = true;
	}
	
	/*
	
		Disables the TX interrupt line at the Linux IRQ subsystem level.
		irqs_table[1] → TX interrupt number (index 1 = TX).

	nosync means:

		Do not wait for any currently running interrupt handlers
		Disable immediately bcoz We are already inside an interrupt handler

		and marking  **tx_irq_disabled  = true ** for cpu to keep state check
									 */
	


	napi_schedule(&cpsw->napi_tx);  // mapped to cpsw_tx_poll fucntion through netif_napi_add()
	return IRQ_HANDLED;
}




1
void __napi_schedule(struct napi_struct *n)
{
	unsigned long flags;

	local_irq_save(flags);
	____napi_schedule(this_cpu_ptr(&softnet_data), n);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(__napi_schedule);


==================
 present in dev.c
==================

bool napi_schedule_prep(struct napi_struct *n)
{
	unsigned long val, new;

	do {
		val = READ_ONCE(n->state);
		if (unlikely(val & NAPIF_STATE_DISABLE))
			return false;
		new = val | NAPIF_STATE_SCHED;

		/* Sets STATE_MISSED bit if STATE_SCHED was already set
		 * This was suggested by Alexander Duyck, as compiler
		 * emits better code than :
		 * if (val & NAPIF_STATE_SCHED)
		 *     new |= NAPIF_STATE_MISSED;
		 */
		new |= (val & NAPIF_STATE_SCHED) / NAPIF_STATE_SCHED *
						   NAPIF_STATE_MISSED;
	} while (cmpxchg(&n->state, val, new) != val);

	return !(val & NAPIF_STATE_SCHED);
}
EXPORT_SYMBOL(napi_schedule_prep);


2
================
present in dev.c
================


/* Called with irq disabled */
static inline void ____napi_schedule(struct softnet_data *sd,
				     struct napi_struct *napi)
{
	list_add_tail(&napi->poll_list, &sd->poll_list);
	__raise_softirq_irqoff(NET_RX_SOFTIRQ);
	
	//sd->poll_list? sd = softnet_data (per CPU), Each CPU has its own poll list, No cross-CPU contention
}


3

================
present in dev.c
================

static __latent_entropy void net_rx_action(struct softirq_action *h)
{
	struct softnet_data *sd = this_cpu_ptr(&softnet_data);
	unsigned long time_limit = jiffies +
		usecs_to_jiffies(READ_ONCE(netdev_budget_usecs));  /*Compute time limit, Softirq is allowed to run only for netdev_budget_usecs secs.
															so that it dont monopolize the cpu. It is ~2 Jiffies. (1 jiffy = 1/60 sec) */
	int budget = READ_ONCE(netdev_budget);  //Max number of packets (or work units) this softirq run  is allowed to process
	LIST_HEAD(list);
	LIST_HEAD(repoll);

	local_irq_disable();
	list_splice_init(&sd->poll_list, &list);  
	local_irq_enable();
	
	/*
	
	1️⃣ local_irq_disable();

		✔️ Disables hardware interrupts on THIS CPU only

		This means: No NIC interrupt can run, No new NAPI instance can be scheduled, sd->poll_list is now stable
		⚠️ Softirqs are already running, so this is safe.

	2️⃣ list_splice_init(&sd->poll_list, &list);

		This is the key line. What list_splice_init() does: 

		It does two things atomically:
		
			>>Moves all elements from sd->poll_list into list (list is teh local copy to store al teh napi instance until now.)
	 		>>Re-initializes sd->poll_list to be empty


	3️⃣ local_irq_enable(); ✔️ Interrupts are back on Now: New NAPI schedules can happen again, 
	They go into sd->poll_list, which is now empty_dequeue

										*/
										
										
   // infinity loops that handles polling and only gets out on two conditions 1) list is empty and there is not other pending work left to do
	for (;;) {
		struct napi_struct *n;

		if (list_empty(&list)) {
			if (!sd_has_rps_ipi_waiting(sd) && list_empty(&repoll))
				goto out;
			break;
		}
		// n -> represent the device to which the napi instance belongs to
		n = list_first_entry(&list, struct napi_struct, poll_list);
		budget -= napi_poll(n, &repoll);   // go to the napi_poll fucntion and see whats happening there.
		
		/*
		Step 1: Pick a NAPI instance
					n = list_first_entry(&list, struct napi_struct, poll_list);

			What this does

			Selects one NAPI instance
			FIFO order
			Fair scheduling between devices

			Only one NAPI runs at a time on this CPU.

		Step 2: Poll the NAPI	budget -= napi_poll(n, &repoll);

			What napi_poll() does

				Calls the driver’s ->poll() function
				Allows it to process up to n->weight
				Returns actual work done
				*/
		
		

		/* If softirq window is exhausted then punt.
		 * Allow this to run for 2 jiffies since which will allow
		 * an average latency of 1.5/HZ.
		 */
		if (unlikely(budget <= 0 ||   time_after_eq(jiffies, time_limit))) {
			sd->time_squeeze++;
			break;
		}		
		// if either budget == 0 or the time limit is reaached...the work is stopped..rest work will be rescheduled later.
	}



//  block below handles the rescheduling if teh work is still left adds the list to repoll and raise the NET_RX_SOFTIRQ again with repoll list.

	local_irq_disable();

	list_splice_tail_init(&sd->poll_list, &list);
	list_splice_tail(&repoll, &list);
	list_splice(&list, &sd->poll_list);
	if (!list_empty(&sd->poll_list))
		__raise_softirq_irqoff(NET_RX_SOFTIRQ);

	net_rps_action_and_irq_enable(sd);
	

out:

	__kfree_skb_flush();
}


4
================
present in dev.c
================


static int napi_poll(struct napi_struct *n, struct list_head *repoll)
{
	void *have;
	int work; //work → for storing number of packets (or descriptors) processed by this poll.
	int weight; // weight → the maximum budget for this poll (from n->weight).

	list_del_init(&n->poll_list);  /* removes this NAPI instance from the list atomically and re-initializes the node.
									This prevents the same NAPI instance from being polled twice while it is being processed.  */

	have = netpoll_poll_lock(n);  /* What this lock is:
		Used to avoid races between normal NAPI polling and netpoll (console) polling.
		Some NAPI instances may also be polled by netpoll_poll() if the system is running low-level debugging (like sending logs via network).
		The lock ensures that only one poller touches this NAPI instance at a time. */

	weight = n->weight;

	/* This NAPI_STATE_SCHED test is for avoiding a race
	 * with netpoll's poll_napi().  Only the entity which
	 * obtains the lock and sees NAPI_STATE_SCHED set will
	 * actually make the ->poll() call.  Therefore we avoid
	 * accidentally calling ->poll() when NAPI is not scheduled.
	 */
	work = 0;
	if (test_bit(NAPI_STATE_SCHED, &n->state)) {
			work = n->poll(n, weight);   // for Tx this becomes :  work = cpsw_tx_poll(n, weight);
		trace_napi_poll(n, work, weight);
	}
	/*  it calls the poll fucntion for tx -> cpsw_tx_poll and then also traces and  Records: 
			Which NAPI instance was polled

			How much work was done
			Its weight (budget)
			Useful for tuning NAPI, detecting misbehaving drivers, or debugging network stalls.		*/

	if (unlikely(work > weight))
		pr_err_once("NAPI poll function %pS returned %d, exceeding its budget of %d.\n",
			    n->poll, work, weight);

	if (likely(work < weight))
		goto out_unlock;

	/* Drivers must not modify the NAPI state if they
	 * consume the entire weight.  In such cases this code
	 * still "owns" the NAPI instance and therefore can
	 * move the instance around on the list at-will.
	 */
	if (unlikely(napi_disable_pending(n))) {   //hecks if napi_disable() was called by the driver while this NAPI was running.
		napi_complete(n);//Clears NAPI_STATE_SCHED, Marks NAPI as idle, Lets it be scheduled again later only if napi_schedule() is called
		goto out_unlock;  
	}

	if (n->gro_bitmask) {
		/* flush too old packets
		 * If HZ < 1000, flush all packets.
		 */
		napi_gro_flush(n, HZ >= 1000);
	}

	gro_normal_list(n);

	/* Some drivers may have called napi_schedule
	 * prior to exhausting their budget.
	 
	 Kernel warns with pr_warn_once because:

		Polling already consumed full budget.
		Rescheduling happened, so remaining work will be deferred.
	 */
	if (unlikely(!list_empty(&n->poll_list))) {
		pr_warn_once("%s: Budget exhausted after napi rescheduled\n",
			     n->dev ? n->dev->name : "backlog");
		goto out_unlock;
	}

	list_add_tail(&n->poll_list, repoll);

out_unlock:
	netpoll_poll_unlock(have); // unlocking the lock held in the start for two pollers to rpevent from the same list.

	return work;  // to net_rx_action()
}


5
==============
in cpsw_priv.c
==============


int cpsw_tx_poll(struct napi_struct *napi_tx, int budget)
{
	struct cpsw_common *cpsw = napi_to_cpsw(napi_tx);
	int num_tx;
	/* 
	 napi_to_cpsw(napi_tx) → converts the napi_struct pointer to the CPSW driver’s private structure.

		Why:
		struct cpsw_common 
		contains all state for the CPSW device:

				TX channels
				IRQ info
				Registers
		This allows the poll function to access the hardware state.

		int num_tx;→ number of descriptors actually processed in this poll.*/
	

	num_tx = cpdma_chan_process(cpsw->txv[0].ch, budget);  // num->tx = how many descriptors were actually processed. 
	if (num_tx < budget) {
		napi_complete(napi_tx);
		writel(0xff, &cpsw->wr_regs->tx_en);  //🛑️tx interrupts are enabled here
		if (cpsw->tx_irq_disabled) {
			cpsw->tx_irq_disabled = false;   // making tge irq_disabled field false that was earlier made true.
			enable_irq(cpsw->irqs_table[1]);
		}
	}

	return num_tx;  // to napi_poll()
}


6
==============
in davinci .c
==============

int cpdma_chan_process(struct cpdma_chan *chan, int quota)  //quota here is budget
{
	int used = 0, ret = 0;

	if (chan->state != CPDMA_STATE_ACTIVE)
		return -EINVAL;

	while (used < quota) {
		ret = __cpdma_chan_process(chan);
		if (ret < 0)
			break;
		used++;
	}
	return used;
}

7
==============
in davinci .c
==============


static int __cpdma_chan_process(struct cpdma_chan *chan)
{
	struct cpdma_ctlr		*ctlr = chan->ctlr;
	struct cpdma_desc __iomem	*desc;
	int				status, outlen;
	int				cb_status = 0;
	struct cpdma_desc_pool		*pool = ctlr->pool;
	dma_addr_t			desc_dma;
	unsigned long			flags;

	spin_lock_irqsave(&chan->lock, flags);
	
	desc = chan->head;
	if (!desc) {
		chan->stats.empty_dequeue++;
		status = -ENOENT;
		goto unlock_ret;
	}
	desc_dma = desc_phys(pool, desc);		//converting cpu pointer to dma-addr

	status	= desc_read(desc, hw_mode);		// reading the mode field of the desc -> owner bit, length, EOQ, completion flags
	outlen	= status & 0x7ff;				// finding the packet length
	if (status & CPDMA_DESC_OWNER) {		// 🛑️checks if hardware is still the owner (1 -> hardware, 0 -> CPU)
		chan->stats.busy_dequeue++;
		status = -EBUSY;
		goto unlock_ret;
	}

	if (status & CPDMA_DESC_PASS_CRC)
		outlen -= CPDMA_DESC_CRC_LEN;

	status	= status & (CPDMA_DESC_EOQ | CPDMA_DESC_TD_COMPLETE |
			    CPDMA_DESC_PORT_MASK | CPDMA_RX_VLAN_ENCAP);

	chan->head = desc_from_phys(pool, desc_read(desc, hw_next));
	chan_write(chan, cp, desc_dma);     /* DMA completeion pointer (cp) register is written so that hardware knows software has processed it
										The CP register is a per-channel hardware register used by the CPDMA engine.
										What does hardware do when CP is written?
											Hardware now knows: “Descriptors up to this address are no longer in use by software.
											  So it can: Mark those descriptors as free, Reuse them for future DMA, ”
											  
											  CP is a boundary marker, not a queue. 
											  
											  */	
	chan->count--;							
	chan->stats.good_dequeue++;

	if ((status & CPDMA_DESC_EOQ) && chan->head) {  //if the hardware reached EOQ and software has added more desc meanwhile so write the hdp
		chan->stats.requeue++;
		chan_write(chan, hdp, desc_phys(pool, chan->head));
	}  


	spin_unlock_irqrestore(&chan->lock, flags);
	if (unlikely(status & CPDMA_DESC_TD_COMPLETE))
		cb_status = -ENOSYS;
	else
		cb_status = status;

	__cpdma_chan_free(chan, desc, outlen, cb_status);
	return status;

unlock_ret:
	spin_unlock_irqrestore(&chan->lock, flags);
	return status;
}


/*
		EXPLANATION OF cpdma_chan_process  &   __cpdma_chan_process
	---------------------------------------------------------------
	
After cpsw_tx_poll() is called from napi_poll(), the handling goes into cpdma_chan_process().
This function is responsible for processing completed TX DMA descriptors up to the given quota (budget). It maintains a counter called used, which represents the number of DMA descriptors that have been successfully completed by the hardware and reclaimed by the software during this poll cycle.

There is a while loop that runs as long as **used < quota**. Inside this loop, __cpdma_chan_process() is called, which processes exactly one DMA descriptor per call.

Inside __cpdma_chan_process(), everytime it processes the descriptor that is head of the channel, pointed to by chan->head, which is the current descriptor expected to be completed next. It reads the descriptor’s hardware OWNER bit from the mode field. If the OWNER bit is set, it means the hardware DMA engine still owns the descriptor and is actively transmitting, so it is not safe for the CPU to touch it. In this case, processing stops and control returns to the caller.

If the OWNER bit is not set, it means the hardware has completed the DMA operation and it is now safe for the CPU to reclaim the descriptor. The function then extracts the transmitted packet length, adjusts it if the CRC was included, and advances the DMA ring by moving chan->head to the next desc hw_next field. It also updates the DMA completion pointer (CP) register to inform the hardware that this descriptor has been fully processed by software.

At this point, the descriptor is considered successfully completed, so the internal descriptor count is decremented and DMA statistics are updated.

If the descriptor has the EOQ (End Of Queue) bit set and there is still another descriptor present in the chain, it means the DMA engine stopped because it reached the end of the programmed queue even though more descriptors were already added into the channel so it again writes HDP (Head Descriptor Pointer) register to restart the DMA engine and continue transmission, preventing a TX stall.

After releasing the channel lock, the function determines the appropriate callback status and then calls __cpdma_chan_free()
*/

8
==============
in davinci .c
==============



static void __cpdma_chan_free(struct cpdma_chan *chan,
			      struct cpdma_desc __iomem *desc,
			      int outlen, int status)
{
	struct cpdma_ctlr		*ctlr = chan->ctlr;
	struct cpdma_desc_pool		*pool = ctlr->pool;
	dma_addr_t			buff_dma;
	int				origlen;
	uintptr_t			token;

	token      = desc_read(desc, sw_token);
	origlen    = desc_read(desc, sw_len);

	buff_dma   = desc_read(desc, sw_buffer);
	if (origlen & CPDMA_DMA_EXT_MAP) {
		origlen &= ~CPDMA_DMA_EXT_MAP;
		dma_sync_single_for_cpu(ctlr->dev, buff_dma, origlen,
					chan->dir);
	} else {
		dma_unmap_single(ctlr->dev, buff_dma, origlen, chan->dir);
	}

	cpdma_desc_free(pool, desc, 1);
	(*chan->handler)((void *)token, outlen, status);  //now this handler is set to cpsw_tx_handler in probe when creating the channel.
}


/* 
 					**Explanation of __cpdma_chan_free()
 				------------------------------------------

After the hardware clears the descriptor’s OWNER bit, indicating that ownership has transitioned from the hardware DMA engine to the CPU, the descriptor is passed to __cpdma_chan_free() for final cleanup.

Inside __cpdma_chan_free(), the function first retrieves the DMA controller (ctlr) and reads the software-maintained fields stored in the descriptor. These fields include the original buffer length of the Ethernet frame and the DMA address of the buffer that was previously mapped using dma_map_single() (or a related DMA mapping API used when the descriptor was prepared).

Next, the function checks whether the buffer was mapped using an external DMA mapping, which is indicated by the CPDMA_DMA_EXT_MAP flag. This flag means that the DMA mapping was not created by this driver and that the buffer already existed in a DMA-mapped state (for example, pre-mapped memory or shared DMA-capable memory). In this case, th_not unmap the buffer, but instead performs dma_sync_single_for_cpu() to ensure cache coherency so that the CPU can safely access the buffer contents.

If the buffer was not externally mapped, then it was mapped by the driver itself using dma_map_single(), and therefore it must be properly released using dma_unmap_single(). This removes the DMA mapping and ensures there are no cache incoherency issues or DMA resource leaks.

After this step, the DMA engine no longer has any association with the buffer, and although the descriptor may still contain the old DMA address value, that address is no longer valid or usable.

The descriptor itself is then returned to the descriptor pool using cpdma_desc_free(pool, desc, 1), making it available for reuse in future transmissions. At this point, the descriptor is fully reclaimed, and its contents are considered invalid until it is reinitialized for a new packet.

Finally, the function calls the upper-layer completion handler (cpsw_tx_handler() for TX), passing the packet token, the number of bytes transmitted, and the transmission status. This notifies the network stack that the packet has been transmitted, allowing it to free the SKB or XDP frame, update transmission statistics, and wake the transmit queue if required.

At the end of this process, both the DMA buffer and the descriptor have been fully released, and the TX completion for that packet is complete.

 */



9/* 
Finally, the function calls the upper-layer completion handler (cpsw_tx_handler() for TX), passing the packet token, the number of bytes transmitted, and the transmission status. This notifies the network stack that the packet has been transmitted, allowing it to free the SKB or XDP frame, update transmission statistics, and wake the transmit queue if required.*/
===========
in dev.c
===========
void cpsw_tx_handler(void *token, int len, int status)
{
	struct cpsw_meta_xdp	*xmeta;
	struct xdp_frame	*xdpf;
	struct net_device	*ndev;
	struct netdev_queue	*txq;
	struct sk_buff		*skb;
	int			ch;

	if (cpsw_is_xdpf_handle(token)) {
		xdpf = cpsw_handle_to_xdpf(token);
		xmeta = (void *)xdpf + CPSW_XMETA_OFFSET;
		ndev = xmeta->ndev;
		ch = xmeta->ch;
		xdp_return_frame(xdpf);
	} else {
		skb = token;
		ndev = skb->dev;
		ch = skb_get_queue_mapping(skb);
		cpts_tx_timestamp(ndev_to_cpsw(ndev)->cpts, skb);
		dev_kfree_skb_any(skb);
	}

	/* Check whether the queue is stopped due to stalled tx dma, if the
	 * queue is stopped then start the queue as we have free desc for tx
	 */
	txq = netdev_get_tx_queue(ndev, ch);
	if (unlikely(netif_tx_queue_stopped(txq)))
		netif_tx_wake_queue(txq);

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += len;
}

/*     
	cpsw_tx_handler()is the TX completion callback registered by the CPSW driver with the CPDMA TX channel, and it is invoked after the hardware has finished transmitting a packet and the corresponding DMA descriptor has been reclaimed and freed. The function receives an opaque token that represents the transmitted packet, along with the number of bytes txdp_return_frame(). Otherwise, the token is treated as a normal socket buffer (sk_buff), from which the network device and TX queue are obtained; hardware transmit timestamping is handled if enabled, and the SKB is safely released using dev_kfree_skb_any(), which defers freeing to the appropriate context if necessary. After freeing the packet memory, the function checks whether the corresponding TX queue was previously stopped due to lack of available TX descriptors, and if so, wakes the queue to allow new packets to be added.

		*/

10
=========
in dev.c
=========

enum skb_free_reason {
			SKB_REASON_CONSUMED,
			SKB_REASON_DROPPED,
		};

static inline void dev_kfree_skb_any(struct sk_buff *skb)
{
	__dev_kfree_skb_any(skb, SKB_REASON_DROPPED);
}

/*in here why SKB_REASON_DROPPED, because dev_kfree_skb_any is a helper function and does not know what */

void __dev_kfree_skb_any(struct sk_buff *skb, enum skb_free_reason reason)
{		
	if (in_irq() || irqs_disabled())
		__dev_kfree_skb_irq(skb, reason);
	else
		dev_kfree_skb(skb);
}
EXPORT_SYMBOL(__dev_kfree_skb_any);


/* A SKB can be freed from many contexts:

		Hard IRQ (TX interrupt)
		Softirq (NET_TX_SOFTIRQ)
		Process context (system call, kthread)
		
		💥 Freeing an SKB incorrectly in the wrong context can crash the kernel. so linux provides this 
		
		
		it checks if the irq are enabled or disabled if any og the thing is true then it cannot free the skb immediately as kfree may sleep
		or acquire lock and in softirq no sleeping should be there so it calls __dev_kfree_skb_irq. 
		
		and if called form the process context it just frees the skb immdiately using dev_kfree_skb.
		
		
		enum skb_free_reason {
			SKB_REASON_CONSUMED,
			SKB_REASON_DROPPED,
		};
		
		
		*/

11
==========
in dev.c
==========


void __dev_kfree_skb_irq(struct sk_buff *skb, enum skb_free_reason reason)
{
	unsigned long flags;

	if (unlikely(!skb))
		return;

	if (likely(refcount_read(&skb->users) == 1)) {    // if ref = 1, this means the current context holds the last reference to the SKB.
		smp_rmb();			//is executed to ensure that all prior reads of SKB data complete before the reference count is set to zero.
		refcount_set(&skb->users, 0);
	} else if (likely(!refcount_dec_and_test(&skb->users))) {  //if more than 1 the function just simply decrements the ref count.
		return;						
	}
	get_kfree_skb_cb(skb)->reason = reason;  //enters the reason on control buffer which will be later be used for tracing and debugging
	local_irq_save(flags);     // stops all interrupts on the current cpu & save the previous interrupt state into flags
	skb->next = __this_cpu_read(softnet_data.completion_queue);  //completion queue is a singly linked list and its head is made skb->next
	__this_cpu_write(softnet_data.completion_queue, skb);		//write skb as the head of the queue.
	raise_softirq_irqoff(NET_TX_SOFTIRQ);   // raises the softirq to defer the work of freeing the skb
	local_irq_restore(flags);
}

//  					------------------------softirq of NET_RX_SOFTIRQ ends here--------------------------

/* 
									Explanation of  __dev_kfree_skb_irq()
								---------------------------------------------

__dev_kfree_skb_irq() is the function that ultimately handles freeing an sk_buff when the free request occurs in IRQ context or with interrupts disabled.
dev_kfree_skb_any() selects this function when it detects that the caller is running in interrupt context or when IRQs_cannot be freed immediately in these contexts.

Inside __dev_kfree_skb_irq(), the function first checks whether the skb pointer is valid. It then examines the reference count (skb->users):

	>If the reference count is exactly 1, this means the current context holds the last reference to the SKB.
	 A read memory barrier smp_rmb()) is executed to ensure that all prior reads of SKB data complete before the reference count is set to
	 zero. This prevents memory reordering issues on SMP systems.
 	 The reference count is then set to 0, marking the SKB as logically dead.

	>If the reference count is greater than 1, the function simply decrements it.
	 If other references still exist, the SKB must not be freed, so the function returns immediately.

Once the SKB is confirmed to be freeable, the reason for freeing is stored in the SKB’s control buffer.

The SKB is then added to a per-CPU completion queue (softnet_data.completion_queue).
This is done by using skb->next to link the SKB into a singly linked list of SKBs waiting to be freed:

skb->next is set to the current head of the completion queue

the completion queue head is then set to the current skb.



Finally, NET_TX_SOFTIRQ is raised. This schedules net_tx_action() to run later in softirq context, where the SKBs in the completion queue are actually freed safely.

🛑️Also it does this as it does not assume that it is alsready isnside a softirq context as __dev_kfree_skb_irq() can be called by hard irq context
softirq context(which is in thi case)/ It does not performs checks of if it is present in softirq context coz that cause check cyles.

This deferred-free mechanism ensures that SKBs are not freed directly in interrupt context, avoids locking and memory allocation in IRQ handlers, and provides safe and efficient cleanup on SMP systems

local_irq_restore(flags)  does:  

 */

12
========
in dev.c
========


static __latent_entropy void net_tx_action(struct softirq_action *h)
{
	struct softnet_data *sd = this_cpu_ptr(&softnet_data);

	if (sd->completion_queue) {
		struct sk_buff *clist;

		local_irq_disable();
		clist = sd->completion_queue;
		sd->completion_queue = NULL;
		local_irq_enable();

		while (clist) {
			struct sk_buff *skb = clist;

			clist = clist->next;

			WARN_ON(refcount_read(&skb->users));  
			if (likely(get_kfree_skb_cb(skb)->reason == SKB_REASON_CONSUMED))	 // this is done for backward compatibilty.
				trace_consume_skb(skb);
			else
				trace_kfree_skb(skb, net_tx_action);  //Emits a tracepoint indicating that an SKB was successfully it records : SKB pointer,
													 // Freeing function name, Reason (dropped), Device info

			if (skb->fclone != SKB_FCLONE_UNAVAILABLE)
				__kfree_skb(skb);
			else
				__kfree_skb_defer(skb);
		}

		__kfree_skb_flush();
	}

	if (sd->output_queue) {
		struct Qdisc *head;

		local_irq_disable();
		head = sd->output_queue;
		sd->output_queue = NULL;
		sd->output_queue_tailp = &sd->output_queue;
		local_irq_enable();

		rcu_read_lock();

		while (head) {
			struct Qdisc *q = head;
			spinlock_t *root_lock = NULL;

			head = head->next_sched;

			/* We need to make sure head->next_sched is read
			 * before clearing __QDISC_STATE_SCHED
			 */
			smp_mb__before_atomic();

			if (!(q->flags & TCQ_F_NOLOCK)) {
				root_lock = qdisc_lock(q);
				spin_lock(root_lock);
			} else if (unlikely(test_bit(__QDISC_STATE_DEACTIVATED,
						     &q->state))) {
				/* There is a synchronize_net() between
				 * STATE_DEACTIVATED flag being set and
				 * qdisc_reset()/some_qdisc_is_busy() in
				 * dev_deactivate(), so we can safely bail out
				 * early here to avoid data race between
				 * qdisc_deactivate() and some_qdisc_is_busy()
				 * for lockless qdisc.
				 */
				clear_bit(__QDISC_STATE_SCHED, &q->state);
				continue;
			}

			clear_bit(__QDISC_STATE_SCHED, &q->state);
			qdisc_run(q);
			if (root_lock)
				spin_unlock(root_lock);
		}

		rcu_read_unlock();
	}

	xfrm_dev_backlog(sd);
}

/*  
		net_tx_action() is the handler for the NET_TX_SOFTIRQ and is responsible for completing all deferred transmit-side work that cannot be safely executed in interrupt context. The function operates on per-CPU data stored in struct softnet_data to avoid global locking and improve scalability. It first checks the per-CPU completion_queue, which holds socket buffers (sk_buff) that were previously queued for deferred freeing by functions such as __dev_kfree_skb_irq(). With local interrupts temporarily disabled, the function atomically detaches the entire completion queue into a local list and clears the per-CPU pointer to prevent races with producers. It then iterates over the list using the skb->next pointer, verifies that each SKB has a zero reference count, emits appropriate trace events based on the free reason, and frees the SKB either immediately using __kfree_skb() for complex or cloned buffers or via __kfree_skb_defer() for simple, non-cloned buffers to allow batched memory reclamation; once all queued SKBs have been processed, __kfree_skb_flush() is called to complete any deferred frees. After SKB cleanup, the function processes the per-CPU output_queue, which contains traffic control (qdisc) instances that were scheduled earlier but could not run immediately; again, with interrupts briefly disabled, the queue is detached, and each qdisc is executed under appropriate locking and RCU protection using qdisc_run() to resume packet transmission through the traffic control layer. Finally, the function invokes xfrm_dev_backlog()to process any pending IPsec transform backlog associated with transmit operations, ensuring that all deferred TX cleanup, scheduling, and protocol work for this CPU is completed before returning from the softirq.
		
		
		MY explanation() : 
		
		This function processes the per-CPU TX completion queue, which contains SKBs that were deferred for freeing from interrupt context. It walks the queue one SKB at a time, verifies that the reference count is zero, and then frees the SKB. For simple, non-cloned SKBs it uses __kfree_skb_defer() to batch the memory free, while for cloned or more complex SKBs it uses __kfree_skb() to free them immediately. After all SKBs in the completion queue are handled, the function flushes any deferred frees.

While running, the function also processes any pending qdiscs stored in the per-CPU output queue by calling qdisc_run(), which allows the traffic control layer to resume transmitting packets by pushing them down toward the device’s ndo_start_xmit() path.
 */






/*							 Explanation of all the below functions()
							------------------------------------------------

First, _kfree_skb_defer() retrieves a per-CPU napi_alloc_cache, which is a small cache used to batch SKB frees on a per-CPU basis. It then calls skb_release_all(), which performs all necessary cleanup except freeing the struct sk_buff itself. This includes running any SKB destructors, releasing protocol and socket state, unmapping and freeing the packet data buffer, releasing page-backed fragments, clearing zero-copy state, and freeing the SKB’s data head. At this point, the packet payload and all associated resources are completely released, and only the empty struct sk_buff object remains.

After the packet contents are released, the now-empty SKB shell is stored in the per-CPU skb_cache array and the cache count is incremented. This means the SKB object is not freed immediately, but queued locally on the same CPU for later bulk freeing. If the cache reaches its maximum size (NAPI_SKB_CACHE_SIZE), all cached SKB objects are freed at once using kmem_cache_free_bulk(), which returns them to the slab allocator efficiently and with minimal locking. Any remaining cached SKBs will be freed later when the cache is flushed.

In summary, this flow immediately frees everything that is large or externally visible (packet data, fragments, DMA-related state, protocol ownership) while deferring only the freeing of the small SKB object itself, allowing the kernel to batch slab frees, reduce allocator overhead, and improve cache locality. The end result is functionally identical to __kfree_skb(), but optimized for high-rate TX paths.  
*/

void __kfree_skb_defer(struct sk_buff *skb)
{
	_kfree_skb_defer(skb);
}




static inline void _kfree_skb_defer(struct sk_buff *skb)
{
	struct napi_alloc_cache *nc = this_cpu_ptr(&napi_alloc_cache);

	/* drop skb->head and call any destructors for packet */
	skb_release_all(skb);

	/* record skb to CPU local list */
	nc->skb_cache[nc->skb_count++] = skb;

#ifdef CONFIG_SLUB
	/* SLUB writes into objects when freeing */
	prefetchw(skb);
#endif

	/* flush skb_cache if it is filled */
	if (unlikely(nc->skb_count == NAPI_SKB_CACHE_SIZE)) {
		kmem_cache_free_bulk(skbuff_head_cache, NAPI_SKB_CACHE_SIZE,
				     nc->skb_cache);
		nc->skb_count = 0;
	}
	
}
	
	
	
/* Free everything but the sk_buff shell. */
static void skb_release_all(struct sk_buff *skb)
{
	skb_release_head_state(skb);
	if (likely(skb->head))
		skb_release_data(skb);
}


static void skb_release_data(struct sk_buff *skb)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	int i;

	if (skb->cloned &&
	    atomic_sub_return(skb->nohdr ? (1 << SKB_DATAREF_SHIFT) + 1 : 1,
			      &shinfo->dataref))
		return;

	for (i = 0; i < shinfo->nr_frags; i++)
		__skb_frag_unref(&shinfo->frags[i]);

	if (shinfo->frag_list)
		kfree_skb_list(shinfo->frag_list);

	skb_zcopy_clear(skb, true);
	skb_free_head(skb);
	
}


/*    

Explanation after ethernet frame is sent :

The DMA engine uses the start-of-packet and end-of-packet information provided in the TX descriptor to read exactly the specified number of bytes from system memory and stream those bytes into the MAC’s transmit FIFO. The DMA engine itself only transfers raw frame data and does not operate at the bit level or handle Ethernet framing details. Once data is available in the MAC FIFO, the MAC hardware takes over and transmits the frame on the wire by first sending a 7-byte Ethernet preamble, which provides a regular transition pattern used by the receiver PHY to recover and synchronize its clock. This is followed by a 1-byte Start Frame Delimiter (SFD) with the pattern 10101011, where the final two consecutive ones indicate the precise boundary between the preamble and the Ethernet frame. After the SFD, the MAC transmits the actual Ethernet frame bytes from the FIFO, appends the CRC, and completes the transmission. On the receive side, detection of the SFD tells the MAC that the next byte corresponds to the start of the Ethernet frame header.

Ohy is used to convert these bytes into elctrical signals and send to the wire.



Preamble :  

The DMA engine uses the start-of-packet and end-of-packet information provided in the TX descriptor to read exactly the specified number of bytes from system memory and stream those bytes into the MAC’s transmit FIFO. The DMA engine itself only transfers raw frame data and does not operate at the bit level or handle Ethernet framing details. Once data is available in the MAC FIFO, the MAC hardware takes over and transmits the frame on the wire by first sending a 7-byte Ethernet preamble, which provides a regular transition pattern used by the receiver PHY to recover and synchronize its clock. This is followed by a 1-byte Start Frame Delimiter (SFD) with the pattern 10101011, where the final two consecutive ones indicate the precise boundary between the preamble and the Ethernet frame. After the SFD, the MAC transmits the actual Ethernet frame bytes from the FIFO, appends the CRC, and completes the transmission. On the receive side, detection of the SFD tells the MAC that the next byte corresponds to the start of the Ethernet frame header.


👉 Yes — the preamble is still sent on modern Ethernet.
👉 It has never gone away.
👉 What changed over time is how it is encoded and handled, not whether it exists.

		1. Is preamble still sent today?

		✔ Yes, for all IEEE Ethernet standards:

		10 Mbps (10BASE-T)
		100 Mbps (Fast Ethernet)
		1 Gbps (Gigabit Ethernet)
		10G / 25G / 40G / 100G Ethernet
		Every Ethernet frame still begins with:
		Preamble + SFD

		This is mandatory per IEEE 802.3.

		2. Why it can feel like “old Ethernet behavior”

		You don’t see the preamble anymore because:

		NICs hide it completely
		Packet captures don’t show it
		Software never touches it
		Modern PHYs lock very fast

		So it feels invisible — but it’s there.

*/




