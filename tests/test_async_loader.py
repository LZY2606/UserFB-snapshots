import asyncio
import inspect
import unittest
from dataclasses import dataclass
from typing import Any

from parsel import Selector

from itemloaders import ItemLoader
from itemloaders.async_loader import AsyncItemLoader
from itemloaders.processors import Identity, MapCompose, TakeFirst

HTML = """
<html>
<body>
<div id="us"><span class="name">Color TV</span><span class="price">$1,200.00</span></div>
<div id="de"><span class="name">Fernseher</span><span class="price">1.200,00 €</span></div>
<p id="summary">the price is $1200</p>
</body>
</html>
"""

JSON = '{"name": "Color TV", "price": "1200"}'


class Gate:
    """A manually released checkpoint for asynchronous processors.

    A processor awaits ``gate.wait()``; the test decides when it may
    continue with ``gate.release()`` or ``gate.fail()``. ``entered`` is set
    as soon as the processor is actually waiting, so tests never need to
    sleep to observe intermediate states.
    """

    def __init__(self) -> None:
        self.entered = asyncio.Event()
        self._future: asyncio.Future = asyncio.get_running_loop().create_future()

    async def wait(self) -> Any:
        self.entered.set()
        return await self._future

    def reset(self) -> None:
        self.entered.clear()
        self._future = asyncio.get_running_loop().create_future()

    def release(self, value: Any = None) -> None:
        self._future.set_result(value)

    def fail(self, exc: BaseException) -> None:
        self._future.set_exception(exc)


class AsyncMethodShapeTest(unittest.TestCase):
    def test_processing_methods_are_coroutine_functions(self):
        for method_name in (
            "add_value",
            "replace_value",
            "add_xpath",
            "replace_xpath",
            "add_css",
            "replace_css",
            "add_jmes",
            "replace_jmes",
            "get_xpath",
            "get_css",
            "get_jmes",
            "get_value",
            "get_output_value",
            "load_item",
        ):
            self.assertTrue(
                inspect.iscoroutinefunction(getattr(AsyncItemLoader, method_name)),
                method_name,
            )

    def test_nested_methods_stay_synchronous(self):
        self.assertFalse(inspect.iscoroutinefunction(AsyncItemLoader.nested_css))
        self.assertFalse(inspect.iscoroutinefunction(AsyncItemLoader.nested_xpath))

    def test_sync_loader_methods_stay_synchronous(self):
        for method_name in ("add_value", "add_css", "get_value", "load_item"):
            self.assertFalse(
                inspect.iscoroutinefunction(getattr(ItemLoader, method_name)),
                method_name,
            )


class LoadItemAtomicityTest(unittest.IsolatedAsyncioTestCase):
    async def test_failed_output_publishes_nothing_and_retry_works(self):
        gates = {"name": Gate(), "price": Gate()}

        async def name_out_proc(values):
            await gates["name"].wait()
            return [v.upper() for v in values]

        async def price_out_proc(values):
            await gates["price"].wait()
            return sum(values)

        class ProductLoader(AsyncItemLoader):
            name_out = staticmethod(name_out_proc)
            price_out = staticmethod(price_out_proc)

        loader = ProductLoader()
        await loader.add_value("name", ["a", "b"])
        await loader.add_value("price", [10, 20])
        item = loader.item
        self.assertEqual(item, {})

        load_task = asyncio.create_task(loader.load_item())
        # names finish first, prices are still waiting: nothing is published
        gates["name"].release()
        await gates["price"].entered.wait()
        self.assertEqual(item, {})

        # prices fail: the item and the collected raw values stay unchanged
        boom = RuntimeError("boom")
        gates["price"].fail(boom)
        with self.assertRaises(ValueError) as cm:
            await load_task
        self.assertIn("Error with output processor", str(cm.exception))
        self.assertIs(cm.exception.__cause__, boom)
        self.assertEqual(item, {})
        self.assertEqual(loader.get_collected_values("name"), ["a", "b"])
        self.assertEqual(loader.get_collected_values("price"), [10, 20])

        # retrying starts from the same inputs and publishes both fields at once
        gates["name"].reset()
        gates["price"].reset()
        retry_task = asyncio.create_task(loader.load_item())
        gates["name"].release()
        await gates["price"].entered.wait()
        self.assertEqual(item, {})
        gates["price"].release()
        item = await retry_task
        self.assertEqual(item, {"name": ["A", "B"], "price": 30})

    async def test_cancelled_load_item_publishes_nothing(self):
        gate = Gate()

        async def name_out_proc(values):
            await gate.wait()
            return values

        class ProductLoader(AsyncItemLoader):
            name_out = staticmethod(name_out_proc)

        loader = ProductLoader()
        await loader.add_value("name", "x")
        item = loader.item

        load_task = asyncio.create_task(loader.load_item())
        await gate.entered.wait()
        load_task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await load_task
        self.assertEqual(item, {})
        self.assertEqual(loader.get_collected_values("name"), ["x"])

    async def test_repeated_load_item_uses_raw_values(self):
        async def name_out_proc(values):
            return values + ["!"]

        class ProductLoader(AsyncItemLoader):
            name_out = staticmethod(name_out_proc)

        loader = ProductLoader()
        await loader.add_value("name", "x")
        self.assertEqual((await loader.load_item())["name"], ["x", "!"])
        # the previous output is not fed back as input
        self.assertEqual((await loader.load_item())["name"], ["x", "!"])
        self.assertEqual(loader.get_collected_values("name"), ["x"])


class AddReplaceAtomicityTest(unittest.IsolatedAsyncioTestCase):
    async def test_failed_add_keeps_previous_values(self):
        gate = Gate()

        async def blocking_upper(values):
            await gate.wait()
            return [v.upper() for v in values]

        loader = AsyncItemLoader()
        await loader.add_value("name", "old")
        await loader.add_value("other", "untouched")

        add_task = asyncio.create_task(loader.add_value("name", ["new"], blocking_upper))
        await gate.entered.wait()
        # the call is still being processed: the buffer is unchanged
        self.assertEqual(loader.get_collected_values("name"), ["old"])

        boom = RuntimeError("boom")
        gate.fail(boom)
        with self.assertRaises(ValueError) as cm:
            await add_task
        self.assertIs(cm.exception.__cause__, boom)
        self.assertEqual(loader.get_collected_values("name"), ["old"])
        self.assertEqual(loader.get_collected_values("other"), ["untouched"])

        # retrying starts from the same input
        gate.reset()
        add_task = asyncio.create_task(loader.add_value("name", ["new"], blocking_upper))
        await gate.entered.wait()
        gate.release()
        await add_task
        self.assertEqual(loader.get_collected_values("name"), ["old", "NEW"])

    async def test_cancelled_replace_keeps_old_value(self):
        gate = Gate()

        async def blocking(values):
            await gate.wait()
            return values

        loader = AsyncItemLoader()
        await loader.add_value("name", "old")

        replace_task = asyncio.create_task(
            loader.replace_value("name", ["new"], blocking)
        )
        await gate.entered.wait()
        replace_task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await replace_task
        self.assertEqual(loader.get_collected_values("name"), ["old"])
        item = await loader.load_item()
        self.assertEqual(item, {"name": ["old"]})

    async def test_failed_input_processor_keeps_old_value(self):
        state = {"fail": False}

        async def maybe_fail(values):
            if state["fail"]:
                raise RuntimeError("input boom")
            return [v.strip() for v in values]

        class ProductLoader(AsyncItemLoader):
            name_in = staticmethod(maybe_fail)

        loader = ProductLoader()
        await loader.add_value("name", " old ")
        state["fail"] = True
        with self.assertRaises(ValueError) as cm:
            await loader.replace_value("name", "new")
        self.assertIn("Error with input processor", str(cm.exception))
        self.assertIsInstance(cm.exception.__cause__, RuntimeError)
        self.assertEqual(loader.get_collected_values("name"), ["old"])

    async def test_multi_field_add_is_atomic(self):
        async def b_in_proc(values):
            raise RuntimeError("boom")

        class ProductLoader(AsyncItemLoader):
            b_in = staticmethod(b_in_proc)

        loader = ProductLoader()
        with self.assertRaises(ValueError):
            await loader.add_value(None, {"a": "1", "b": "2"})
        self.assertEqual(loader.get_collected_values("a"), [])
        self.assertEqual(loader.get_collected_values("b"), [])

    async def test_multi_field_add_value(self):
        loader = AsyncItemLoader()
        result = await loader.add_value(None, {"name": "foo", "price": "5"})
        self.assertIs(result, loader)
        self.assertEqual(loader.get_collected_values("name"), ["foo"])
        self.assertEqual(loader.get_collected_values("price"), ["5"])


class MixedProcessorsTest(unittest.IsolatedAsyncioTestCase):
    async def test_sync_and_async_processors_combined(self):
        def sync_strip(values):
            return [v.strip() for v in values]

        async def async_upper(values):
            return [v.upper() for v in values]

        def context_suffix(values, loader_context):
            return [v + loader_context["suffix"] for v in values]

        loader = AsyncItemLoader(suffix="!")
        await loader.add_value(
            "name", [" a ", "b "], sync_strip, async_upper, context_suffix
        )
        self.assertEqual(loader.get_collected_values("name"), ["A!", "B!"])

    async def test_async_field_processors_with_loader_context(self):
        async def name_in_proc(values, loader_context):
            return [v + loader_context["mark"] for v in values]

        async def name_out_proc(values):
            return values[0]

        class ProductLoader(AsyncItemLoader):
            name_in = staticmethod(name_in_proc)
            name_out = staticmethod(name_out_proc)

        loader = ProductLoader(mark="?")
        await loader.add_value("name", "foo")
        self.assertEqual(loader.get_collected_values("name"), ["foo?"])
        item = await loader.load_item()
        self.assertEqual(item, {"name": "foo?"})

    async def test_get_value_and_get_output_value(self):
        async def async_double(values):
            return [v * 2 for v in values]

        loader = AsyncItemLoader()
        value = await loader.get_value(["a", "b"], async_double)
        self.assertEqual(value, ["aa", "bb"])

        await loader.add_value("name", ["x"])
        self.assertEqual(await loader.get_output_value("name"), ["x"])

    async def test_awaitables_inside_values_are_not_awaited(self):
        inner = asyncio.get_running_loop().create_future()
        inner.set_result("inner")

        async def wrap(values):
            return [inner]

        loader = AsyncItemLoader()
        await loader.add_value("name", "x", wrap)
        collected = loader.get_collected_values("name")
        self.assertIs(collected[0], inner)

    async def test_sync_compose_processors_keep_working(self):
        loader = AsyncItemLoader()
        await loader.add_value(
            "name", ["hello", "world"], MapCompose(str.upper, lambda v: v[0])
        )
        self.assertEqual(loader.get_collected_values("name"), ["H", "W"])


class EmptyAndRegexTest(unittest.IsolatedAsyncioTestCase):
    async def test_none_values_are_dropped(self):
        loader = AsyncItemLoader()
        await loader.add_value("name", None)
        self.assertEqual(loader.get_collected_values("name"), [])
        await loader.replace_value("name", None)
        self.assertEqual(loader.get_collected_values("name"), [])

        async def to_none(values):
            return None

        await loader.add_value("name", "x", to_none)
        self.assertEqual(loader.get_collected_values("name"), [])

    async def test_empty_processed_values_are_dropped(self):
        async def to_empty(values):
            return []

        class ProductLoader(AsyncItemLoader):
            name_in = staticmethod(to_empty)

        loader = ProductLoader()
        await loader.add_value("name", "x")
        self.assertEqual(loader.get_collected_values("name"), [])

    async def test_regex_extraction_with_async_processor(self):
        async def upper(value):
            return value.upper()

        loader = AsyncItemLoader()
        await loader.add_value("name", "name: foo", TakeFirst(), upper, re="name: (.+)")
        self.assertEqual(loader.get_collected_values("name"), ["FOO"])

    async def test_regex_extraction_from_selector(self):
        loader = AsyncItemLoader(selector=Selector(text=HTML))
        value = await loader.get_css("#summary::text", TakeFirst(), re=r"the price is (.*)")
        self.assertEqual(value, "$1200")
        await loader.add_xpath("price", '//p[@id="summary"]/text()', re=r"\$(\d+)")
        self.assertEqual(loader.get_collected_values("price"), ["1200"])


class SelectorMethodsTest(unittest.IsolatedAsyncioTestCase):
    async def test_add_get_replace_css(self):
        loader = AsyncItemLoader(selector=Selector(text=HTML))
        await loader.add_css("name", "#us .name::text")
        self.assertEqual(loader.get_collected_values("name"), ["Color TV"])
        self.assertEqual(await loader.get_css("#de .name::text"), ["Fernseher"])
        await loader.replace_css("name", "#de .name::text")
        self.assertEqual(loader.get_collected_values("name"), ["Fernseher"])

    async def test_add_get_replace_xpath(self):
        loader = AsyncItemLoader(selector=Selector(text=HTML))
        await loader.add_xpath("name", '//div[@id="us"]/span[@class="name"]/text()')
        self.assertEqual(loader.get_collected_values("name"), ["Color TV"])
        self.assertEqual(
            await loader.get_xpath('//div[@id="de"]/span[@class="name"]/text()'),
            ["Fernseher"],
        )
        await loader.replace_xpath("name", '//div[@id="de"]/span[@class="name"]/text()')
        self.assertEqual(loader.get_collected_values("name"), ["Fernseher"])

    async def test_add_get_replace_jmes(self):
        if not hasattr(Selector, "jmespath"):
            self.skipTest("parsel >= 1.8.1 is required for JMESPath support")
        loader = AsyncItemLoader(selector=Selector(text=JSON))
        await loader.add_jmes("name", "name")
        self.assertEqual(loader.get_collected_values("name"), ["Color TV"])
        self.assertEqual(await loader.get_jmes("price"), ["1200"])
        await loader.replace_jmes("name", "price")
        self.assertEqual(loader.get_collected_values("name"), ["1200"])

    async def test_selector_methods_support_async_processors(self):
        async def upper_list(values):
            return [v.upper() for v in values]

        loader = AsyncItemLoader(selector=Selector(text=HTML))
        await loader.add_css("name", "#us .name::text", upper_list)
        self.assertEqual(loader.get_collected_values("name"), ["COLOR TV"])
        value = await loader.get_xpath(
            '//div[@id="de"]/span[@class="name"]/text()', upper_list
        )
        self.assertEqual(value, ["FERNSEHER"])

    async def test_selector_method_without_selector_raises(self):
        loader = AsyncItemLoader()
        with self.assertRaises(RuntimeError):
            await loader.add_css("name", "b::text")


class NestedLoaderTest(unittest.IsolatedAsyncioTestCase):
    async def test_nested_loaders_with_regional_rules(self):
        async def us_price(values):
            return [float(v.replace("$", "").replace(",", "")) for v in values]

        async def de_price(values):
            return [
                float(v.replace(" €", "").replace(".", "").replace(",", "."))
                for v in values
            ]

        loader = AsyncItemLoader(selector=Selector(text=HTML))
        us = loader.nested_css("#us")
        de = loader.nested_xpath('//div[@id="de"]')

        # nested loaders are created synchronously and share the item
        self.assertIsInstance(us, AsyncItemLoader)
        self.assertIsInstance(de, AsyncItemLoader)
        self.assertIs(us.item, loader.item)
        self.assertIs(de.item, loader.item)

        await us.add_css("price_us", "span.price::text", us_price)
        await de.add_css("price_de", "span.price::text", de_price)
        await us.add_xpath("name", 'span[@class="name"]/text()')

        item = await loader.load_item()
        self.assertEqual(item["price_us"], [1200.0])
        self.assertEqual(item["price_de"], [1200.0])
        self.assertEqual(item["name"], ["Color TV"])

    async def test_nested_loader_does_not_recollect_item_values(self):
        class ProductLoader(AsyncItemLoader):
            default_output_processor = TakeFirst()

        loader = ProductLoader(selector=Selector(text=HTML))
        await loader.add_css("name", "#us .name::text")
        item = await loader.load_item()
        self.assertEqual(item["name"], "Color TV")

        child = loader.nested_css("#de")
        # the values already published to the item are not collected again
        self.assertEqual(child.get_collected_values("name"), ["Color TV"])
        await child.add_css("name", ".name::text")
        self.assertEqual(child.get_collected_values("name"), ["Color TV", "Fernseher"])
        item = await child.load_item()
        self.assertEqual(item["name"], "Color TV")

    async def test_load_item_uses_the_calling_loader_context(self):
        async def name_out_proc(values, loader_context):
            return [loader_context["region"] + ":" + v for v in values]

        class ProductLoader(AsyncItemLoader):
            name_out = staticmethod(name_out_proc)

        loader = ProductLoader(selector=Selector(text=HTML), region="root")
        child = loader.nested_css("#us", region="child")
        await child.add_css("name", "span.name::text")
        self.assertEqual((await child.load_item())["name"], ["child:Color TV"])
        self.assertEqual((await loader.load_item())["name"], ["root:Color TV"])


class IndependentLoadersTest(unittest.IsolatedAsyncioTestCase):
    async def test_root_loaders_do_not_share_state(self):
        loader1 = AsyncItemLoader(unit="cm")
        loader2 = AsyncItemLoader(unit="inch")
        await loader1.add_value("name", "a")
        await loader2.add_value("name", "b")
        await loader2.add_value("name", "c")

        self.assertEqual(loader1.get_collected_values("name"), ["a"])
        self.assertEqual(loader2.get_collected_values("name"), ["b", "c"])
        self.assertEqual(loader1.context["unit"], "cm")
        self.assertEqual(loader2.context["unit"], "inch")
        loader1.context["extra"] = True
        self.assertNotIn("extra", loader2.context)

        self.assertEqual(await loader1.load_item(), {"name": ["a"]})
        self.assertEqual(await loader2.load_item(), {"name": ["b", "c"]})
        self.assertIsNot(loader1.item, loader2.item)


@dataclass
class Product:
    name: Any = None
    price: Any = None


class DataclassItemTest(unittest.IsolatedAsyncioTestCase):
    async def test_dataclass_item(self):
        async def to_float(values):
            return [float(v) for v in values]

        class ProductLoader(AsyncItemLoader):
            default_item_class = Product
            price_in = staticmethod(to_float)
            name_out = TakeFirst()
            price_out = TakeFirst()

        loader = ProductLoader()
        await loader.add_value("name", "tv")
        await loader.add_value("price", "9.5")
        item = await loader.load_item()
        self.assertEqual(item, Product(name="tv", price=9.5))

    async def test_given_dataclass_item(self):
        loader = AsyncItemLoader(item=Product(name="tv"))
        await loader.add_value("price", "5")
        item = await loader.load_item()
        self.assertIsInstance(item, Product)
        self.assertEqual(item.name, ["tv"])
        self.assertEqual(item.price, ["5"])


class SyncRegressionTest(unittest.TestCase):
    """The synchronous ItemLoader keeps working as before."""

    def test_sync_loader_basics(self):
        loader = ItemLoader(selector=Selector(text=HTML))
        loader.add_css("name", "#us .name::text")
        loader.add_xpath("price", '//p[@id="summary"]/text()', re=r"\$(\d+)")
        loader.add_value("stock", "3")
        nested = loader.nested_css("#de")
        nested.add_css("name", ".name::text")
        item = loader.load_item()
        self.assertEqual(
            item,
            {
                "name": ["Color TV", "Fernseher"],
                "price": ["1200"],
                "stock": ["3"],
            },
        )

    def test_sync_loader_processors(self):
        loader = ItemLoader()
        loader.add_value("name", ["hello", "world"], MapCompose(str.upper))
        self.assertEqual(loader.get_collected_values("name"), ["HELLO", "WORLD"])
        self.assertEqual(loader.get_output_value("name"), ["HELLO", "WORLD"])
        loader.default_output_processor = TakeFirst()
        self.assertEqual(loader.load_item(), {"name": "HELLO"})

    def test_sync_loader_identity_default(self):
        loader = ItemLoader()
        self.assertIsInstance(loader.default_input_processor, Identity)
        loader.add_value("name", "x")
        self.assertEqual(loader.load_item(), {"name": ["x"]})


if __name__ == "__main__":
    unittest.main()
