package com.example.javatest.service;

import com.example.javatest.entity.Book;
import java.util.List;

public interface BookService {

    List<Book> findAll();

    Book findById(Long id);

    void add(Book book);

    void update(Book book);

    void deleteById(Long id);
}
