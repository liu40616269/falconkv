from setuptools import setup, find_packages

setup(
    name="pyfalconkv",
    version="0.1.0",
    packages=find_packages(),
    package_data={
        "pyfalconkv": ["*.so"],
    },
    python_requires=">=3.10",
)
