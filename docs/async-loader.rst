.. _async-loader:

Asynchronous Item Loader
========================

.. module:: itemloaders.async_loader
   :synopsis: Asynchronous Item Loader

Sometimes cleaning a field requires waiting for an asynchronous resource,
for example a local dictionary that is only exposed through an async API.
Putting an ``async`` processor into the synchronous :class:`ItemLoader`
does not work: the coroutine object itself would be stored as the field
value.

For those cases, ``itemloaders`` provides an explicitly asynchronous
entry point::

    from itemloaders.async_loader import AsyncItemLoader

    loader = AsyncItemLoader(selector=selector)
    await loader.add_css("name", "b::text")
    item = await loader.load_item()

Every method that runs processors is a coroutine and must be awaited:
``add_value``, ``replace_value``, ``add_xpath``, ``replace_xpath``,
``add_css``, ``replace_css``, ``add_jmes``, ``replace_jmes``,
``get_xpath``, ``get_css``, ``get_jmes``, ``get_value``,
``get_output_value`` and ``load_item``. Everything else — the constructor,
:meth:`~itemloaders.ItemLoader.get_collected_values` and the nested
loader methods — stays synchronous.

Processors are executed in the same order as in the synchronous loader:
first the processors passed to the method call, then the field input
processor, and at output time the field output processor. Each processor
may be a regular function or return an awaitable; only the value returned
by the processor itself is awaited, awaitables nested inside returned
lists are left untouched. The usual rules still apply: ``None`` values
are dropped, single values are wrapped in lists, the ``re`` parameter
extracts data with a regular expression, and field metadata and
``loader_context`` are passed as before.

.. note:: The built-in :class:`~itemloaders.processors.Compose` and
    :class:`~itemloaders.processors.MapCompose` processors remain
    synchronous and do not await asynchronous functions nested inside
    them. Use a single awaitable processor that handles the whole list
    of values instead.

:meth:`~itemloaders.ItemLoader.nested_xpath` and
:meth:`~itemloaders.ItemLoader.nested_css` return an
:class:`AsyncItemLoader` immediately, without awaiting. The nested loader
shares the item and the collected values with its parent, processes input
with its own selector and context, and does not collect again the values
that the item already holds. Output processing during ``load_item`` uses
the context of the loader on which ``load_item`` is called. Separate root
loaders never share collected values or contexts.

Errors and cancellation are safe to retry: an ``add``/``replace`` call
finishes all of its processing before touching the collected values, and
``load_item`` computes the output of every field before populating the
item. If a processor raises, a :exc:`ValueError` is raised with the
original exception as its ``__cause__``, and both the item and the
previously collected values are left unchanged, so the call can simply be
retried with the same input. :class:`asyncio.CancelledError` propagates
unchanged.

Operations on a single loader tree are meant to be awaited one after
another; concurrently modifying the same loader tree is not supported.
The asynchronous loader never starts or manages an event loop itself, it
must be used from code that is already running inside one.

The synchronous :class:`~itemloaders.ItemLoader` is not affected by any
of this and keeps its blocking interface.
