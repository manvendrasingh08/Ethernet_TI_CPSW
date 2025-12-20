
irqreturn_t cpsw_tx_interrupt(int irq, void *dev_id)
{
	struct cpsw_common *cpsw = dev_id;

	writel(0, &cpsw->wr_regs->tx_en);   // disable the tx interrupts by putting 0 in tx_en register.
	cpdma_ctlr_eoi(cpsw->dma, CPDMA_EOI_TX);  // it provides the eacknowledgement that the interrupt ha sbeen recieved to teh source.

	if (cpsw->quirk_irq) {
		disable_irq_nosync(cpsw->irqs_table[1]);
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
	


	napi_schedule(&cpsw->napi_tx);
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



// below block handles the rescheduling if teh work is still left adds the list to repoll and raise the NET_RX_SOFTIRQ again with repoll list.

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
	int work, weight;

	list_del_init(&n->poll_list);

	have = netpoll_poll_lock(n);

	weight = n->weight;

	/* This NAPI_STATE_SCHED test is for avoiding a race
	 * with netpoll's poll_napi().  Only the entity which
	 * obtains the lock and sees NAPI_STATE_SCHED set will
	 * actually make the ->poll() call.  Therefore we avoid
	 * accidentally calling ->poll() when NAPI is not scheduled.
	 */
	work = 0;
	if (test_bit(NAPI_STATE_SCHED, &n->state)) {
			work = n->poll(n, weight);
		trace_napi_poll(n, work, weight);
	}

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
	if (unlikely(napi_disable_pending(n))) {
		napi_complete(n);
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
	 */
	if (unlikely(!list_empty(&n->poll_list))) {
		pr_warn_once("%s: Budget exhausted after napi rescheduled\n",
			     n->dev ? n->dev->name : "backlog");
		goto out_unlock;
	}

	list_add_tail(&n->poll_list, repoll);

out_unlock:
	netpoll_poll_unlock(have);

	return work;
}


5
==============
in cpsw_priv.c
==============


int cpsw_tx_poll(struct napi_struct *napi_tx, int budget)
{
	struct cpsw_common *cpsw = napi_to_cpsw(napi_tx);
	int num_tx;

	num_tx = cpdma_chan_process(cpsw->txv[0].ch, budget);
	if (num_tx < budget) {
		napi_complete(napi_tx);
		writel(0xff, &cpsw->wr_regs->tx_en);
		if (cpsw->tx_irq_disabled) {
			cpsw->tx_irq_disabled = false;
			enable_irq(cpsw->irqs_table[1]);
		}
	}

	return num_tx;
}


6
==============
in davinci .c
==============

int cpdma_chan_process(struct cpdma_chan *chan, int quota)
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
	desc_dma = desc_phys(pool, desc);

	status	= desc_read(desc, hw_mode);
	outlen	= status & 0x7ff;
	if (status & CPDMA_DESC_OWNER) {
		chan->stats.busy_dequeue++;
		status = -EBUSY;
		goto unlock_ret;
	}

	if (status & CPDMA_DESC_PASS_CRC)
		outlen -= CPDMA_DESC_CRC_LEN;

	status	= status & (CPDMA_DESC_EOQ | CPDMA_DESC_TD_COMPLETE |
			    CPDMA_DESC_PORT_MASK | CPDMA_RX_VLAN_ENCAP);

	chan->head = desc_from_phys(pool, desc_read(desc, hw_next));
	chan_write(chan, cp, desc_dma);
	chan->count--;
	chan->stats.good_dequeue++;

	if ((status & CPDMA_DESC_EOQ) && chan->head) {
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
	(*chan->handler)((void *)token, outlen, status);
}

now this handler is set to cpsw_tx_handler in probe when creating the channel.

so here it is: 


9
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

10
=========
in dev.c
=========


void __dev_kfree_skb_any(struct sk_buff *skb, enum skb_free_reason reason)
{
	if (in_irq() || irqs_disabled())
		__dev_kfree_skb_irq(skb, reason);
	else
		dev_kfree_skb(skb);
}
EXPORT_SYMBOL(__dev_kfree_skb_any);


11
==========
in dev.c
==========


void __dev_kfree_skb_irq(struct sk_buff *skb, enum skb_free_reason reason)
{
	unsigned long flags;

	if (unlikely(!skb))
		return;

	if (likely(refcount_read(&skb->users) == 1)) {
		smp_rmb();
		refcount_set(&skb->users, 0);
	} else if (likely(!refcount_dec_and_test(&skb->users))) {
		return;
	}
	get_kfree_skb_cb(skb)->reason = reason;
	local_irq_save(flags);
	skb->next = __this_cpu_read(softnet_data.completion_queue);
	__this_cpu_write(softnet_data.completion_queue, skb);
	raise_softirq_irqoff(NET_TX_SOFTIRQ);
	local_irq_restore(flags);
}

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
			if (likely(get_kfree_skb_cb(skb)->reason == SKB_REASON_CONSUMED))
				trace_consume_skb(skb);
			else
				trace_kfree_skb(skb, net_tx_action);

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
