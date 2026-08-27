#include "sk_e1000_ring.h"

/*
 * Advance one descriptor position and wrap at the end of the ring.
 *
 * The caller is expected to provide an index within [0, count - 1].
 *
 * A zero-sized ring or an out-of-range index has no valid next
 * position, so return zero defensively. Public helpers that depend
 * on index validity perform their own validation before calling this
 * function.
 */
unsigned int sk_e1000_ring_next(unsigned int index,
                                unsigned int count)
{
    if (count == 0U)
        return 0U;

    if (index >= count)
        return 0U;

    if (index == (count - 1U))
        return 0U;

    return index + 1U;
}

/*
 * Producer == consumer is the unambiguous empty state.
 *
 * This is why the design reserves one descriptor as a guard slot:
 * the same producer/consumer relationship is never also used to
 * represent a full ring.
 */
int sk_e1000_ring_empty(unsigned int producer,
                        unsigned int consumer)
{
    return producer == consumer;
}

/*
 * The ring is full when the producer cannot advance without reaching
 * the consumer.
 *
 * Rings with fewer than two slots have zero usable capacity because
 * one slot must remain reserved as the guard slot.
 *
 * Invalid indexes are treated as unavailable rather than allowing
 * malformed ring state to masquerade as a valid full condition.
 */
int sk_e1000_ring_full(unsigned int producer,
                       unsigned int consumer,
                       unsigned int count)
{
    if (count < 2U)
        return 1;

    if (producer >= count || consumer >= count)
        return 0;

    return sk_e1000_ring_next(producer, count) == consumer;
}

/*
 * Count descriptors between consumer and producer.
 *
 * No-wrap case:
 *
 *     consumer          producer
 *         |                 |
 *         v                 v
 *       [ C ][ x ][ x ][ P ]
 *
 *     used = producer - consumer
 *
 * Wraparound case:
 *
 *       producer             consumer
 *          |                    |
 *          v                    v
 *       [ P ][ ][ ][ ][ ][ ][ C ][ x ]
 *
 *     used = (count - consumer) + producer
 */
unsigned int sk_e1000_ring_used(unsigned int producer,
                                unsigned int consumer,
                                unsigned int count)
{
    if (count == 0U)
        return 0U;

    if (producer >= count || consumer >= count)
        return 0U;

    if (producer >= consumer)
        return producer - consumer;

    return (count - consumer) + producer;
}

/*
 * One descriptor is permanently reserved as the guard slot.
 *
 * Therefore:
 *
 *     usable capacity = count - 1
 *     free            = usable capacity - used
 */
unsigned int sk_e1000_ring_free(unsigned int producer,
                                unsigned int consumer,
                                unsigned int count)
{
    unsigned int used;

    if (count < 2U)
        return 0U;

    if (producer >= count || consumer >= count)
        return 0U;

    used = sk_e1000_ring_used(producer, consumer, count);

    return (count - 1U) - used;
}