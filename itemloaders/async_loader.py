"""
Asynchronous Item Loader

See documentation in :ref:`async-loader`.
"""

from __future__ import annotations

import inspect
from typing import TYPE_CHECKING, Any, Self

from itemadapter import ItemAdapter
from parsel import Selector  # noqa: TC002  # for sphinx
from parsel.utils import extract_regex, flatten

from itemloaders import ItemLoader, _StatsCollector
from itemloaders.common import wrap_loader_context
from itemloaders.utils import arg_to_iter

if TYPE_CHECKING:
    from collections.abc import Callable, Iterable
    from re import Pattern


class AsyncItemLoader(ItemLoader):
    """
    An :class:`ItemLoader` whose data-processing methods are awaitable, so
    that processors can perform asynchronous work (e.g. query a local
    asynchronous dictionary) while the collected data is being cleaned.

    Every method that runs processors is a coroutine here and must be
    awaited: :meth:`add_value`, :meth:`replace_value`,
    :meth:`add_xpath`, :meth:`replace_xpath`, :meth:`add_css`,
    :meth:`replace_css`, :meth:`add_jmes`, :meth:`replace_jmes`,
    :meth:`get_xpath`, :meth:`get_css`, :meth:`get_jmes`,
    :meth:`get_value`, :meth:`get_output_value` and :meth:`load_item`.

    Processors are executed in the same order as in :class:`ItemLoader`:
    first the processors passed to the method call, then the field input
    processor, and at output time the field output processor. Each
    processor may return a plain value or an awaitable; only the value
    returned by the processor itself is awaited, awaitables nested inside
    returned lists are left as they are.

    :meth:`nested_xpath` and :meth:`nested_css` stay synchronous and return
    an :class:`AsyncItemLoader` that shares the item and the collected
    values with its parent.

    Operations on a single loader tree are meant to be awaited one after
    another; concurrent modification of the same loader tree is not
    supported.
    """

    def __init__(
        self,
        item: Any = None,
        selector: Selector | None = None,
        parent: ItemLoader | None = None,
        stats: _StatsCollector | None = None,
        **context: Any,
    ):
        if parent is None:
            super().__init__(
                item=item, selector=selector, parent=None, stats=stats, **context
            )
            return
        # A nested loader shares the item and the collected values of its
        # parent. The values already present in the item must not be
        # collected again, so the initialization of ItemLoader (which would
        # append them to the shared buffer) cannot be reused here.
        if item is None:
            item = parent.item
        self.selector: Selector | None = selector
        self.stats: _StatsCollector | None = (
            stats if stats is not None else parent.stats
        )
        context.update(selector=selector)
        self._local_item = item
        context["item"] = item
        self.context = context
        self.parent: ItemLoader | None = parent
        self._local_values: dict[str, list[Any]] = {}

    async def add_value(
        self,
        field_name: str | None,
        value: Any,
        *processors: Callable[..., Any],
        re: str | Pattern[str] | None = None,
        **kw: Any,
    ) -> Self:
        """
        Async version of :meth:`ItemLoader.add_value`.

        The value is fully processed before the collected data of any field
        is modified: if a processor raises or the call is cancelled, the
        previously collected values are left untouched.

        :returns: The current AsyncItemLoader instance for method chaining.
        :rtype: AsyncItemLoader
        """
        value = await self.get_value(value, *processors, re=re, **kw)
        if value is None:
            return self
        if not field_name:
            processed = []
            for k, v in value.items():
                processed.append((k, await self._process_input_value(k, v)))
            for k, v in processed:
                self._commit_value(k, v, replace=False)
        else:
            processed_value = await self._process_input_value(field_name, value)
            self._commit_value(field_name, processed_value, replace=False)
        return self

    async def replace_value(
        self,
        field_name: str | None,
        value: Any,
        *processors: Callable[..., Any],
        re: str | Pattern[str] | None = None,
        **kw: Any,
    ) -> Self:
        """
        Async version of :meth:`ItemLoader.replace_value`.

        The new value is fully processed before it replaces the collected
        data: if a processor raises or the call is cancelled, the
        previously collected values are left untouched.

        :returns: The current AsyncItemLoader instance for method chaining.
        :rtype: AsyncItemLoader
        """
        value = await self.get_value(value, *processors, re=re, **kw)
        if value is None:
            return self
        if not field_name:
            processed = []
            for k, v in value.items():
                processed.append((k, await self._process_input_value(k, v)))
            for k, v in processed:
                self._commit_value(k, v, replace=True)
        else:
            processed_value = await self._process_input_value(field_name, value)
            self._commit_value(field_name, processed_value, replace=True)
        return self

    async def _process_input_value(self, field_name: str, value: Any) -> Any:
        """Run the field input processor on *value*, awaiting it if needed.

        Only processes, does not touch the collected values.
        """
        value = arg_to_iter(value)
        proc = self.get_input_processor(field_name)
        _proc = proc
        proc = wrap_loader_context(proc, self.context)
        try:
            processed = proc(value)
            if inspect.isawaitable(processed):
                processed = await processed
        except Exception as e:
            raise ValueError(
                f"Error with input processor {_proc.__class__.__name__}:"
                f" field={field_name!r} value={value!r} error='{type(e).__name__}: {e!s}'"
            ) from e
        return processed

    def _commit_value(self, field_name: str, processed_value: Any, replace: bool) -> None:
        """Store an already processed value in the collected data."""
        if replace:
            self._values.pop(field_name, None)
        if processed_value:
            self._values.setdefault(field_name, [])
            self._values[field_name] += arg_to_iter(processed_value)

    async def get_value(
        self,
        value: Any,
        *processors: Callable[..., Any],
        re: str | Pattern[str] | None = None,
        **kw: Any,
    ) -> Any:
        """
        Async version of :meth:`ItemLoader.get_value`.

        Each processor may return a plain value or an awaitable; awaitables
        are awaited before the value is passed to the next processor.
        """
        if re:
            value = arg_to_iter(value)
            value = flatten(extract_regex(re, x) for x in value)

        for proc in processors:
            if value is None:
                break
            _proc = proc
            proc = wrap_loader_context(proc, self.context)  # noqa: PLW2901
            try:
                new_value = proc(value)
                if inspect.isawaitable(new_value):
                    new_value = await new_value
            except Exception as e:
                raise ValueError(
                    f"Error with processor {_proc.__class__.__name__} "
                    f"value={value!r} error='{type(e).__name__}: {e!s}'"
                ) from e
            value = new_value
        return value

    async def load_item(self) -> Any:
        """
        Async version of :meth:`ItemLoader.load_item`.

        The output values of all fields are computed first, and only then
        is the item populated: if an output processor raises or the call is
        cancelled, the item is left untouched.
        """
        adapter = ItemAdapter(self.item)
        output = {}
        for field_name in tuple(self._values):
            value = await self.get_output_value(field_name)
            if value is not None:
                output[field_name] = value
        for field_name, value in output.items():
            adapter[field_name] = value

        return adapter.item

    async def get_output_value(self, field_name: str) -> Any:
        """
        Async version of :meth:`ItemLoader.get_output_value`.

        The field output processor may return a plain value or an
        awaitable. This method doesn't populate or modify the item at all.
        """
        proc = self.get_output_processor(field_name)
        proc = wrap_loader_context(proc, self.context)
        value = self._values.get(field_name, [])
        try:
            output = proc(value)
            if inspect.isawaitable(output):
                output = await output
        except Exception as e:
            raise ValueError(
                f"Error with output processor: field={field_name!r} "
                f"value={value!r} error='{type(e).__name__}: {e!s}'"
            ) from e
        return output

    async def add_xpath(
        self,
        field_name: str | None,
        xpath: str | Iterable[str],
        *processors: Callable[..., Any],
        re: str | Pattern[str] | None = None,
        **kw: Any,
    ) -> Self:
        """
        Async version of :meth:`ItemLoader.add_xpath`.

        :returns: The current AsyncItemLoader instance for method chaining.
        :rtype: AsyncItemLoader
        """
        values = self._get_xpathvalues(xpath, field_name, **kw)
        return await self.add_value(field_name, values, *processors, re=re, **kw)

    async def replace_xpath(
        self,
        field_name: str | None,
        xpath: str | Iterable[str],
        *processors: Callable[..., Any],
        re: str | Pattern[str] | None = None,
        **kw: Any,
    ) -> Self:
        """
        Async version of :meth:`ItemLoader.replace_xpath`.

        :returns: The current AsyncItemLoader instance for method chaining.
        :rtype: AsyncItemLoader
        """
        values = self._get_xpathvalues(xpath, field_name, **kw)
        return await self.replace_value(field_name, values, *processors, re=re, **kw)

    async def get_xpath(
        self,
        xpath: str | Iterable[str],
        *processors: Callable[..., Any],
        re: str | Pattern[str] | None = None,
        **kw: Any,
    ) -> Any:
        """Async version of :meth:`ItemLoader.get_xpath`."""
        values = self._get_xpathvalues(xpath, **kw)
        return await self.get_value(values, *processors, re=re, **kw)

    async def add_css(
        self,
        field_name: str | None,
        css: str | Iterable[str],
        *processors: Callable[..., Any],
        re: str | Pattern[str] | None = None,
        **kw: Any,
    ) -> Self:
        """
        Async version of :meth:`ItemLoader.add_css`.

        :returns: The current AsyncItemLoader instance for method chaining.
        :rtype: AsyncItemLoader
        """
        values = self._get_cssvalues(css, field_name)
        return await self.add_value(field_name, values, *processors, re=re, **kw)

    async def replace_css(
        self,
        field_name: str | None,
        css: str | Iterable[str],
        *processors: Callable[..., Any],
        re: str | Pattern[str] | None = None,
        **kw: Any,
    ) -> Self:
        """
        Async version of :meth:`ItemLoader.replace_css`.

        :returns: The current AsyncItemLoader instance for method chaining.
        :rtype: AsyncItemLoader
        """
        values = self._get_cssvalues(css, field_name)
        return await self.replace_value(field_name, values, *processors, re=re, **kw)

    async def get_css(
        self,
        css: str | Iterable[str],
        *processors: Callable[..., Any],
        re: str | Pattern[str] | None = None,
        **kw: Any,
    ) -> Any:
        """Async version of :meth:`ItemLoader.get_css`."""
        values = self._get_cssvalues(css)
        return await self.get_value(values, *processors, re=re, **kw)

    async def add_jmes(
        self,
        field_name: str | None,
        jmes: str | Iterable[str],
        *processors: Callable[..., Any],
        re: str | Pattern[str] | None = None,
        **kw: Any,
    ) -> Self:
        """
        Async version of :meth:`ItemLoader.add_jmes`.

        :returns: The current AsyncItemLoader instance for method chaining.
        :rtype: AsyncItemLoader
        """
        values = self._get_jmesvalues(jmes, field_name)
        return await self.add_value(field_name, values, *processors, re=re, **kw)

    async def replace_jmes(
        self,
        field_name: str | None,
        jmes: str | Iterable[str],
        *processors: Callable[..., Any],
        re: str | Pattern[str] | None = None,
        **kw: Any,
    ) -> Self:
        """
        Async version of :meth:`ItemLoader.replace_jmes`.

        :returns: The current AsyncItemLoader instance for method chaining.
        :rtype: AsyncItemLoader
        """
        values = self._get_jmesvalues(jmes, field_name)
        return await self.replace_value(field_name, values, *processors, re=re, **kw)

    async def get_jmes(
        self,
        jmes: str | Iterable[str],
        *processors: Callable[..., Any],
        re: str | Pattern[str] | None = None,
        **kw: Any,
    ) -> Any:
        """Async version of :meth:`ItemLoader.get_jmes`."""
        values = self._get_jmesvalues(jmes)
        return await self.get_value(values, *processors, re=re, **kw)
