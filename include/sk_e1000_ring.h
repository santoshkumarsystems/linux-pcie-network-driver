#ifndef SK_E1000_RING_H
#define SK_E1000_RING_H

/*
 * Hardware-independent circular-ring helpers.
 *
 * The driver uses one descriptor slot as a guard slot. This makes the
 * producer/consumer states unambiguous:
 *
 *     producer == consumer
 *         -> ring empty
 *
 *     next(producer) == consumer
 *         -> ring full
 *
 * With N descriptor slots, the usable capacity is therefore N - 1.
 *
 * These helpers contain no Linux kernel, DMA, or MMIO dependencies so the
 * same production logic can be exercised directly by user-space unit tests.
 */

/*
 * Return the next descriptor index, wrapping back to zero at the end.
 *
 * Example for count = 4:
 *
 *     0 -> 1 -> 2 -> 3 -> 0
 */
unsigned int sk_e1000_ring_next(unsigned int index,
                                unsigned int count);

/*
 * An empty ring has no descriptors between producer and consumer.
 */
int sk_e1000_ring_empty(unsigned int producer,
                        unsigned int consumer);

/*
 * A ring is full when advancing the producer would collide with the
 * consumer. One slot therefore remains unused as the guard slot.
 */
int sk_e1000_ring_full(unsigned int producer,
                       unsigned int consumer,
                       unsigned int count);

/*
 * Return the number of descriptors currently owned between consumer
 * and producer.
 *
 * Examples for count = 8:
 *
 *     producer=0 consumer=0 -> 0 used
 *     producer=3 consumer=0 -> 3 used
 *     producer=1 consumer=6 -> 3 used  (wraparound)
 */
unsigned int sk_e1000_ring_used(unsigned int producer,
                                unsigned int consumer,
                                unsigned int count);

/*
 * Return the number of descriptors still available to the producer.
 *
 * Because one slot is reserved as the guard slot:
 *
 *     usable capacity = count - 1
 */
unsigned int sk_e1000_ring_free(unsigned int producer,
                                unsigned int consumer,
                                unsigned int count);

#endif /* SK_E1000_RING_H */