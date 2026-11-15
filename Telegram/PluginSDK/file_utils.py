from pathlib import Path

from desktop._runtime import context


def get_plugin_dir():
    return context().directory


def get_plugin_data_dir(account_id=None):
    directory = context().data_directory
    if account_id is not None:
        account_id = str(account_id)
        if not account_id.isdecimal():
            raise ValueError("Некорректный ID аккаунта.")
        directory = directory / "accounts" / account_id
    directory.mkdir(parents=True, exist_ok=True)
    return Path(directory)
