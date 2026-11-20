"""One sandboxed native helper per installation. SDK 3."""
from ._runtime import context


async def start(resource, arguments=(), *, config=""):
    return await context().manager.runtime.request("helper.start", params={
        "resource": resource, "arguments": list(arguments), "config": config})


async def stop():
    return await context().manager.runtime.request("helper.stop")


async def running():
    return await context().manager.runtime.request("helper.status")


async def network_available():
    return await context().manager.runtime.request("helper.network_available")


async def offer_socks5(port, user, password):
    """Offer an authenticated listener on 127.0.0.1 in Ovygram's proxy settings."""
    return await context().manager.runtime.request("helper.offer", params={
        "port": port, "user": user, "password": password})


async def proxy_active():
    return await context().manager.runtime.request("helper.proxy_active")
